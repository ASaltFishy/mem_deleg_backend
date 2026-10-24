#include "include/latency_profile.h"
#include "kernel_bridge.h"
#include "include/memory_delegation_prctl.h"
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

struct md_kernel_bridge {
  md_shared_log *Log;
  uint64_t Session;
  uint32_t Cpu;
};

extern "C" int md_kernel_log_initialize(void *Memory, size_t Bytes, uint32_t Pages) {
  size_t Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (!Memory || reinterpret_cast<uintptr_t>(Memory) % Page || !Bytes ||
      Bytes % Page || Bytes / Page > MD_SHARED_LOG_MAX_PAGES ||
      Bytes <= sizeof(md_shared_log) + 2 * sizeof(md_shared_slot) || !Pages)
    return EINVAL;
  auto *Log = static_cast<md_shared_log *>(Memory);
  // Fresh broker backing is zero-filled; touch only the header, not all slots.
  memset(Memory, 0, sizeof(*Log));
  Log->magic = MD_SHARED_LOG_MAGIC;
  Log->version = MD_SHARED_LOG_VERSION;
  Log->capacity = static_cast<uint32_t>((Bytes - sizeof(*Log)) / (2 * sizeof(md_shared_slot)));
  Log->arena_nr_pages = Pages;
  Log->log_bytes = static_cast<uint32_t>(Bytes);
  return 0;
}

extern "C" int md_kernel_bridge_open(md_kernel_bridge **Out,
                                      const md_kernel_bridge_config *Config) {
  if (!Out || !Config || !Config->ring_memory || !Config->ring_bytes ||
      Config->ring_bytes > UINT32_MAX || Config->arena_cpu > UINT16_MAX)
    return EINVAL;
  *Out = nullptr;
  md_shared_registration Request{};
  Request.size = sizeof(Request);
  Request.version = MD_SHARED_LOG_VERSION;
  Request.log_address = reinterpret_cast<uintptr_t>(Config->ring_memory);
  Request.log_bytes = static_cast<uint32_t>(Config->ring_bytes);
  if (prctl(PR_SET_MEMORY_DELEGATION_LOG, &Request,
            PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND | PR_MEMORY_DELEGATION_F_SHARED_RINGS,
            static_cast<unsigned long>(Config->arena_cpu),
            static_cast<unsigned long>(Config->arena_data_base)) != 0) {
    int Error = errno;
    bool Unsupported = Error == EINVAL || Error == EOPNOTSUPP ||
                       Error == ENOSYS || Error == EPROTONOSUPPORT;
    if (Unsupported && Config->mode == MD_KERNEL_BRIDGE_OPTIONAL) return 0;
    // Preserve the actual syscall errno for required-mode diagnostics.
    return Error;
  }
  void *Memory = mmap(nullptr, sizeof(md_kernel_bridge), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Memory == MAP_FAILED) return errno;
  auto *Bridge = static_cast<md_kernel_bridge *>(Memory);
  Bridge->Log = static_cast<md_shared_log *>(Config->ring_memory);
  Bridge->Session = Request.session;
  Bridge->Cpu = Config->arena_cpu;
  *Out = Bridge;
  return 0;
}

extern "C" uint64_t md_kernel_bridge_session(const md_kernel_bridge *Bridge) {
  return Bridge ? Bridge->Session : 0;
}

// ArenaMutex serializes normal producers, including migrated holders. Selecting
// a reusable slot does not advance the published tail or expose partial records.
static int prepareNormalSlot(md_kernel_bridge *Bridge, uint64_t Session,
                              uint16_t Cpu, md_shared_slot **Out,
                              uint64_t *TailOut) {
  if (!Bridge || Session != Bridge->Session || Cpu != Bridge->Cpu)
    return EINVAL;
  md_shared_log *Log = Bridge->Log;
  if (__atomic_load_n(&Log->state, __ATOMIC_ACQUIRE) != MD_SHARED_LOG_RUNNING)
    return ESHUTDOWN;
  uint64_t Tail = __atomic_load_n(&Log->normal.reserved, __ATOMIC_RELAXED);
  uint64_t Reclaimed = __atomic_load_n(&Log->normal.head, __ATOMIC_ACQUIRE);
  if (Tail < Reclaimed || Tail == UINT64_MAX) return EOVERFLOW;
  if (Tail - Reclaimed >= Log->capacity) return ENOSPC;
  *Out = &Log->slots[Tail % Log->capacity];
  *TailOut = Tail;
  return 0;
}

static void publishNormalSlot(md_kernel_bridge *Bridge, md_shared_slot *Slot,
                               uint64_t Tail) {
  Slot->reserved = 0;
  __atomic_store_n(&Slot->ready, Tail + 1, __ATOMIC_RELAXED);
  __atomic_store_n(&Bridge->Log->normal.reserved, Tail + 1, __ATOMIC_RELEASE);
}

extern "C" int md_kernel_bridge_normal(md_kernel_bridge *Bridge,
                                         const md_shared_record *Record) {
  return md_kernel_bridge_normal_ticket(Bridge, Record, nullptr);
}

extern "C" int md_kernel_bridge_normal_ticket(md_kernel_bridge *Bridge,
    const md_shared_record *Record, uint64_t *Ticket) {
  MD_SCOPE(7, PublishProbe);
  if (!Bridge || !Record) return EINVAL;
  md_shared_slot *Slot;
  uint64_t Tail;
  int Error = prepareNormalSlot(Bridge, Record->session, Record->cpu, &Slot, &Tail);
  if (Error) return Error;
  Slot->record = *Record;
  if (Ticket) *Ticket = Tail + 1;
  publishNormalSlot(Bridge, Slot, Tail);
  return 0;
}

extern "C" int md_kernel_bridge_alloc(md_kernel_bridge *Bridge, uint64_t Session,
                                        uint16_t Cpu, uint64_t BlockId,
                                        uint64_t Generation, uint64_t AllocationId,
                                        uint32_t StartPage, uint32_t NrPages) {
  MD_SCOPE(7, PublishProbe);
  md_shared_slot *Slot;
  uint64_t Tail;
  int Error = prepareNormalSlot(Bridge, Session, Cpu, &Slot, &Tail);
  if (Error) return Error;
  md_shared_record &Record = Slot->record;
  Record.session = Session;
  Record.block_id = BlockId;
  Record.generation = Generation;
  Record.allocation_id = AllocationId;
  Record.start_page = StartPage;
  Record.nr_pages = NrPages;
  Record.cpu = Cpu;
  Record.op = MD_LOG_ALLOC;
  Record.flags = 0;
  Record.reserved = 0;
  publishNormalSlot(Bridge, Slot, Tail);
  return 0;
}

extern "C" int md_kernel_log_remote(md_shared_log *Log,
                                      const md_shared_record *Record, unsigned UserFlags) {
  if (UserFlags & ~static_cast<unsigned>(MD_USER_FREE_CLEAR)) return EINVAL;
  if (!Log || !Record || Record->op != MD_LOG_FREE) return EINVAL;
  if (__atomic_load_n(&Log->state, __ATOMIC_ACQUIRE) != MD_SHARED_LOG_RUNNING)
    return ESHUTDOWN;
  __u64 Tail = __atomic_load_n(&Log->remote.reserved, __ATOMIC_RELAXED);
  for (;;) {
    uint64_t Reclaimed = __atomic_load_n(&Log->remote.reclaimed, __ATOMIC_ACQUIRE);
    if (Tail < Reclaimed || Tail == UINT64_MAX) return EOVERFLOW;
    if (Tail - Reclaimed >= Log->capacity) return ENOSPC;
    if (__atomic_compare_exchange_n(&Log->remote.reserved, &Tail, Tail + 1, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
  }
#ifdef MD_TEST_REMOTE_HOLE
  extern void md_test_remote_reserved(uint64_t);
  md_test_remote_reserved(Tail);
#endif
  md_shared_slot &Slot = Log->slots[Log->capacity + Tail % Log->capacity];
  Slot.record = *Record;
  Slot.reserved = UserFlags;
  __atomic_store_n(&Slot.ready, Tail + 1, __ATOMIC_RELEASE);
  return 0;
}

extern "C" void md_kernel_bridge_close(md_kernel_bridge *Bridge) {
  if (Bridge) munmap(Bridge, sizeof(*Bridge));
}
