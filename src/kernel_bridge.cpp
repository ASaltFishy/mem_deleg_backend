#include "kernel_bridge.h"
#include "include/memory_delegation_prctl.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRingMagic = 0x4d44524cU;
constexpr uint16_t kRingVersion = 1U;

struct KernelLogEntry {
  uint8_t Op;
  uint8_t SrcCpu;
  uint16_t Flags;
  uint32_t StartPage;
  uint32_t NumPages;
};

struct KernelLogRing {
  uint32_t Magic;
  uint16_t Version;
  uint16_t Flags;
  uint32_t Capacity;
  uint32_t ArenaNrPages;
  uint32_t RingSize;
  uint32_t Head;
  uint32_t Tail;
  uint32_t Dropped;
  uint32_t LastError;
  KernelLogEntry Entries[];
};

static_assert(sizeof(KernelLogEntry) == 12, "kernel log-entry ABI drift");
static_assert(offsetof(KernelLogRing, Entries) == 36,
              "kernel log-ring ABI drift");

uint32_t floorPowerOfTwo(uint32_t Value) {
  uint32_t Result = 1;
  while (Result <= Value / 2)
    Result <<= 1;
  return Result;
}

bool unsupportedPrctlError(int Error) {
  return Error == EINVAL || Error == EOPNOTSUPP || Error == ENOSYS;
}

} // namespace

struct md_kernel_bridge {
  KernelLogRing *Ring;
  size_t RingSize;
  uintptr_t ArenaDataBase;
  uint32_t ArenaCpu;
  uint32_t ArenaNrPages;
  uint32_t Channel;
  pid_t Pid;
  pthread_mutex_t ProducerLock;
};

extern "C" int md_kernel_bridge_open(
    md_kernel_bridge **Out, const md_kernel_bridge_config *Config) {
  if (!Out || !Config || !Config->arena_data_base ||
      !Config->arena_nr_pages || Config->arena_cpu > UINT8_MAX ||
      (Config->mode != MD_KERNEL_BRIDGE_OPTIONAL &&
       Config->mode != MD_KERNEL_BRIDGE_REQUIRED))
    return EINVAL;
  *Out = nullptr;

  const long RuntimePageSize = sysconf(_SC_PAGESIZE);
  if (RuntimePageSize <= 0)
    return ENOTSUP;
  void *Storage = mmap(nullptr, sizeof(md_kernel_bridge),
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Storage == MAP_FAILED)
    return errno;
  const size_t RingSize = static_cast<size_t>(RuntimePageSize);
  void *Memory = Config->ring_memory;
  if (Memory && reinterpret_cast<uintptr_t>(Memory) % RingSize) {
    munmap(Storage, sizeof(md_kernel_bridge));
    return EINVAL;
  }
  if (!Memory)
    Memory = mmap(nullptr, RingSize, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Memory == MAP_FAILED) {
    munmap(Storage, sizeof(md_kernel_bridge));
    return errno;
  }

  memset(Memory, 0, RingSize);
  auto *Ring = static_cast<KernelLogRing *>(Memory);
  const size_t HeaderSize = offsetof(KernelLogRing, Entries);
  const uint32_t RawCapacity = static_cast<uint32_t>(
      (RingSize - HeaderSize) / sizeof(KernelLogEntry));
  if (!RawCapacity) {
    if (!Config->ring_memory) munmap(Memory, RingSize);
    munmap(Storage, sizeof(md_kernel_bridge));
    return ENOSPC;
  }
  Ring->Magic = kRingMagic;
  Ring->Version = kRingVersion;
  Ring->Capacity = floorPowerOfTwo(RawCapacity);
  Ring->ArenaNrPages = Config->arena_nr_pages;
  Ring->RingSize = static_cast<uint32_t>(RingSize);

  const unsigned long RegisterFlags =
      PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND |
      PR_MEMORY_DELEGATION_F_ASYNC_EXTERNAL |
      (Config->reset_arena ? PR_MEMORY_DELEGATION_F_RESET_ARENA : 0UL);
  long Result = prctl(PR_SET_MEMORY_DELEGATION_LOG,
                      reinterpret_cast<unsigned long>(Ring), RegisterFlags,
                      static_cast<unsigned long>(Config->arena_cpu),
                      static_cast<unsigned long>(Config->arena_data_base));
  if (Result != 0) {
    const int Error = errno;
    if (!Config->ring_memory) munmap(Memory, RingSize);
    munmap(Storage, sizeof(md_kernel_bridge));
    if (Config->mode == MD_KERNEL_BRIDGE_OPTIONAL &&
        unsupportedPrctlError(Error))
      return 0;
    return unsupportedPrctlError(Error) ? ENOTSUP : Error;
  }

  auto *Bridge = static_cast<md_kernel_bridge *>(Storage);
  Bridge->Ring = Ring;
  Bridge->RingSize = RingSize;
  Bridge->ArenaDataBase = Config->arena_data_base;
  Bridge->ArenaCpu = Config->arena_cpu;
  Bridge->ArenaNrPages = Config->arena_nr_pages;
  Bridge->Channel = Config->channel_id;
  Bridge->Pid = getpid();
  // Registration has succeeded; retain its pinned mapping for the mm lifetime.
  int LockError = pthread_mutex_init(&Bridge->ProducerLock, nullptr);
  if (LockError) {
    munmap(Storage, sizeof(md_kernel_bridge));
    return LockError;
  }
  *Out = Bridge;
  return 0;
}

static int drainLocked(md_kernel_bridge *Bridge) {
  const long Result = prctl(
      PR_SET_MEMORY_DELEGATION_LOG, 0UL,
      PR_MEMORY_DELEGATION_F_DRAIN_LOG,
      static_cast<unsigned long>(Bridge->ArenaCpu),
      static_cast<unsigned long>(Bridge->ArenaDataBase));
  if (Result < 0)
    return errno;
  return static_cast<int>(
      __atomic_load_n(&Bridge->Ring->LastError, __ATOMIC_ACQUIRE));
}

// An mm can have several allocator threads and cross-CPU FREE producers.
// Serialize publication; release Tail only after the entire entry is written.
namespace {
struct ProducerGuard {
  pthread_mutex_t *Mutex;
  explicit ProducerGuard(pthread_mutex_t *M) : Mutex(M) {
    pthread_mutex_lock(Mutex);
  }
  ~ProducerGuard() { pthread_mutex_unlock(Mutex); }
};
} // namespace

extern "C" int md_kernel_bridge_drain(md_kernel_bridge *Bridge) {
  if (!Bridge)
    return EINVAL;
  ProducerGuard Guard(&Bridge->ProducerLock);
  return drainLocked(Bridge);
}

extern "C" uint32_t md_kernel_bridge_pending(md_kernel_bridge *Bridge) {
  if (!Bridge) return 0;
  ProducerGuard Guard(&Bridge->ProducerLock);
  uint32_t Head = __atomic_load_n(&Bridge->Ring->Head, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&Bridge->Ring->Tail, __ATOMIC_ACQUIRE) - Head;
}

extern "C" int md_kernel_bridge_enqueue(
    md_kernel_bridge *Bridge, unsigned Operation, uint32_t StartPage,
    uint32_t NumPages, unsigned Flags, uint32_t *Ticket) {
  if (!Bridge || (Operation != MD_KERNEL_LOG_ALLOC &&
                  Operation != MD_KERNEL_LOG_FREE) ||
      !NumPages || StartPage >= Bridge->ArenaNrPages ||
      NumPages > Bridge->ArenaNrPages - StartPage ||
      (Flags & ~static_cast<unsigned>(MD_KERNEL_LOG_PREFAULT)))
    return EINVAL;

  ProducerGuard Guard(&Bridge->ProducerLock);
  KernelLogRing *Ring = Bridge->Ring;
  // Scheduler-consumed errors are sticky: never erase one on a new append.
  int PendingError = static_cast<int>(
      __atomic_load_n(&Ring->LastError, __ATOMIC_ACQUIRE));
  if (PendingError)
    return PendingError;
  uint32_t Head = __atomic_load_n(&Ring->Head, __ATOMIC_ACQUIRE);
  uint32_t Tail = __atomic_load_n(&Ring->Tail, __ATOMIC_RELAXED);
  if (Tail - Head >= Ring->Capacity) {
    // Match Scudo: let switch-out consume the batch before forcing a drain.
    sched_yield();
    Head = __atomic_load_n(&Ring->Head, __ATOMIC_ACQUIRE);
    int Error = static_cast<int>(
        __atomic_load_n(&Ring->LastError, __ATOMIC_ACQUIRE));
    if (!Error && Tail - Head >= Ring->Capacity)
      Error = drainLocked(Bridge);
    if (Error)
      return Error;
    Head = __atomic_load_n(&Ring->Head, __ATOMIC_ACQUIRE);
    if (Tail - Head >= Ring->Capacity) {
      __atomic_add_fetch(&Ring->Dropped, 1U, __ATOMIC_RELAXED);
      return ENOSPC;
    }
  }

  KernelLogEntry &Entry = Ring->Entries[Tail & (Ring->Capacity - 1)];
  Entry.Op = static_cast<uint8_t>(Operation);
  Entry.SrcCpu = static_cast<uint8_t>(Bridge->ArenaCpu);
  Entry.Flags = static_cast<uint16_t>(Flags);
  Entry.StartPage = StartPage;
  Entry.NumPages = NumPages;
  __atomic_store_n(&Ring->Tail, Tail + 1, __ATOMIC_RELEASE);
  if (Ticket)
    *Ticket = Tail + 1;
  return 0;
}

extern "C" int md_kernel_bridge_transition(
    md_kernel_bridge *Bridge, unsigned Operation, uint32_t StartPage,
    uint32_t NumPages, unsigned Flags) {
  return md_kernel_bridge_enqueue(Bridge, Operation, StartPage, NumPages,
                                  Flags, nullptr);
}

extern "C" uint32_t md_kernel_bridge_channel(const md_kernel_bridge *Bridge) {
  return Bridge->Channel;
}

extern "C" void md_kernel_bridge_close(md_kernel_bridge *Bridge) {
  if (!Bridge)
    return;
  // No unregister prctl exists. Drain before close. Private ring mappings stay
  // alive until exit; shared ring storage belongs to the backend and its slot
  // must not be recycled while kernel or Pending descriptors may reference it.
  // After fork, only the parent owns this inherited channel. The child must
  // neither drain it using its own mm nor lock a mutex inherited while held.
  if (Bridge->Pid == getpid()) {
    (void)md_kernel_bridge_drain(Bridge);
    pthread_mutex_destroy(&Bridge->ProducerLock);
  }
  munmap(Bridge, sizeof(*Bridge));
}
