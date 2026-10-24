#include "scudo_delegation.h"

#include "delegation_backend.h"
#include "atomic_helpers.h"

#include <stdlib.h>
#include <unistd.h>
#include <sched.h>

namespace scudo {
namespace {

atomic_u32 InitState = {};
md_backend *Backend = nullptr;

bool envEnabled(const char *Name, bool DefaultValue) {
  const char *Value = getenv(Name);
  if (Value == nullptr || Value[0] == '\0')
    return DefaultValue;
  return Value[0] != '0';
}

void initialize() {
  u32 Expected = 0;
  if (!atomic_compare_exchange_strong(&InitState, &Expected, 1u,
                                      memory_order_acquire)) {
    while (atomic_load(&InitState, memory_order_acquire) == 1u)
      sched_yield();
    return;
  }
  if (!envEnabled("SCUDO_DELEGATION_BACKEND", false)) {
    atomic_store(&InitState, 3u, memory_order_release);
    return;
  }
  const char *Domain = getenv("MEMORY_DELEGATION_DOMAIN");
  if (Domain == nullptr || Domain[0] == '\0')
    Domain = "allocator_delegation";
  md_config Config{};
  Config.domain = Domain;
#if defined(__ANDROID__) || defined(__aarch64__)
  Config.fixed_address = 0x2000000000ULL;
#else
  Config.fixed_address = 0x280000000000ULL;
#endif
  Config.capacity = MD_DEFAULT_ARENA_CAPACITY;
  Config.reset = envEnabled("MEMORY_DELEGATION_RESET", false) ? 1 : 0;
  if (md_backend_open(&Backend, &Config) != 0)
    Backend = nullptr;
  atomic_store(&InitState, Backend != nullptr ? 2u : 3u,
               memory_order_release);
}

md_backend *getBackend() {
  u32 State = atomic_load(&InitState, memory_order_acquire);
  // The acquire load already observes initialize()'s publication of Backend.
  if (LIKELY(State == 2u)) return Backend;
  if (State == 3u) return nullptr;
  initialize();
  return atomic_load(&InitState, memory_order_acquire) == 2u ? Backend : nullptr;
}

} // namespace

bool scudoDelegationEnabled() { return getBackend() != nullptr; }

extern "C" void md_scudo_stats(md_stats *Stats) {
  md_get_stats(getBackend(), Stats);
}

bool scudoDelegationContains(uptr Address) {
  md_backend *Current = getBackend();
  return Current != nullptr && md_contains(Current, Address);
}

bool scudoDelegationAcquire(uptr Size, uptr Alignment, uptr HeadersSize,
                            ScudoDelegationLease &Lease,
                            uptr &EntryHeaderPosition) {
  md_backend *Current = getBackend();
  if (Current == nullptr)
    return false;
  md_lease Acquired{};
  uptr PageSize = getPageSizeCached();
  if (md_acquire(Current, Size + HeadersSize + Alignment,
                 Max(Alignment, PageSize),
                 md_process_id(), &Acquired) != 0)
    return false;
  EntryHeaderPosition = roundUp(Acquired.address + HeadersSize, Alignment) -
                        HeadersSize;
  if (EntryHeaderPosition < Acquired.address ||
      EntryHeaderPosition + HeadersSize + Size >
          Acquired.address + Acquired.size) {
    (void)md_release(Current, &Acquired, 0, nullptr);
    return false;
  }
  Lease.BlockId = Acquired.block_id;
  Lease.Owner = Acquired.owner;
  Lease.Generation = Acquired.generation;
  Lease.AllocationId = Acquired.allocation_id;
  Lease.KernelSession = Acquired.kernel_session;
  Lease.Base = Acquired.address;
  Lease.Size = Acquired.size;
  return true;
}

bool scudoDelegationRelease(const ScudoDelegationLease &Lease) {
  if (Lease.Owner != md_process_id()) return false;
  md_backend *Current = getBackend();
  if (Current == nullptr)
    return false;
  md_lease Released{};
  Released.block_id = Lease.BlockId;
  Released.owner = Lease.Owner;
  Released.generation = Lease.Generation;
  Released.allocation_id = Lease.AllocationId;
  Released.kernel_session = Lease.KernelSession;
  Released.address = Lease.Base;
  Released.size = Lease.Size;
  return md_release(Current, &Released, 0, nullptr) == 0;
}

} // namespace scudo

#include <errno.h>
extern "C" int md_scudo_checkpoint() {
  return scudo::getBackend() ? md_backend_checkpoint(scudo::getBackend()) : ENODEV;
}
