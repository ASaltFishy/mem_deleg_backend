#include "kernel_bridge.h"
#include "include/memory_delegation_prctl.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
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
  uint32_t Reserved;
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
  void *Memory = mmap(nullptr, RingSize, PROT_READ | PROT_WRITE,
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
    munmap(Memory, RingSize);
    munmap(Storage, sizeof(md_kernel_bridge));
    return ENOSPC;
  }
  Ring->Magic = kRingMagic;
  Ring->Version = kRingVersion;
  Ring->Capacity = floorPowerOfTwo(RawCapacity);
  Ring->ArenaNrPages = Config->arena_nr_pages;
  Ring->RingSize = static_cast<uint32_t>(RingSize);

  const unsigned long RegisterFlags = Config->reset_arena
      ? PR_MEMORY_DELEGATION_F_RESET_ARENA : 0UL;
  long Result = prctl(PR_SET_MEMORY_DELEGATION_LOG,
                      reinterpret_cast<unsigned long>(Ring), RegisterFlags,
                      static_cast<unsigned long>(Config->arena_cpu),
                      static_cast<unsigned long>(Config->arena_data_base));
  if (Result != 0 && Config->reset_arena && unsupportedPrctlError(errno)) {
    Result = prctl(PR_SET_MEMORY_DELEGATION_LOG,
                   reinterpret_cast<unsigned long>(Ring), 0UL,
                   static_cast<unsigned long>(Config->arena_cpu),
                   static_cast<unsigned long>(Config->arena_data_base));
  }
  if (Result != 0) {
    const int Error = errno;
    munmap(Memory, RingSize);
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
  *Out = Bridge;
  return 0;
}

extern "C" int md_kernel_bridge_drain(md_kernel_bridge *Bridge) {
  if (!Bridge)
    return EINVAL;
  const long Result = prctl(
      PR_SET_MEMORY_DELEGATION_LOG, 0UL,
      PR_MEMORY_DELEGATION_F_DRAIN_LOG,
      static_cast<unsigned long>(Bridge->ArenaCpu),
      static_cast<unsigned long>(Bridge->ArenaDataBase));
  return Result < 0 ? errno : 0;
}

extern "C" int md_kernel_bridge_transition(
    md_kernel_bridge *Bridge, unsigned Operation, uint32_t StartPage,
    uint32_t NumPages, unsigned Flags) {
  if (!Bridge || (Operation != MD_KERNEL_LOG_ALLOC &&
                  Operation != MD_KERNEL_LOG_FREE) ||
      !NumPages || StartPage >= Bridge->ArenaNrPages ||
      NumPages > Bridge->ArenaNrPages - StartPage ||
      (Flags & ~static_cast<unsigned>(MD_KERNEL_LOG_PREFAULT)))
    return EINVAL;

  KernelLogRing *Ring = Bridge->Ring;
  uint32_t Head = __atomic_load_n(&Ring->Head, __ATOMIC_ACQUIRE);
  uint32_t Tail = __atomic_load_n(&Ring->Tail, __ATOMIC_RELAXED);
  if (Tail - Head >= Ring->Capacity) {
    int Error = md_kernel_bridge_drain(Bridge);
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
  return md_kernel_bridge_drain(Bridge);
}

extern "C" void md_kernel_bridge_close(md_kernel_bridge *Bridge) {
  if (!Bridge)
    return;
  // There is currently no unregister prctl. Drain and intentionally leave the
  // pinned ring mapping alive until process exit; unmapping could let the VA be
  // reused while the kernel still owns a reference to the page.
  (void)md_kernel_bridge_drain(Bridge);
  munmap(Bridge, sizeof(*Bridge));
}
