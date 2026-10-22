#include "jemalloc_adapter.h"
#include <jemalloc/jemalloc.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

static md_backend *Backend;
static md_lease *LeaseByPage;
static size_t PageSize;
static extent_hooks_t Hooks;
static extent_hooks_t *DefaultHooks;
static thread_local bool PayloadRequest;
extern "C" void md_jemalloc_request(int Enabled) { PayloadRequest = Enabled; }

#if defined(__ANDROID__)
static constexpr uintptr_t kDelegationBase = 0x2000000000ULL;
#else
static constexpr uintptr_t kDelegationBase = 0x280000000000ULL;
#endif

extern "C" int md_jemalloc_owns(void *Address) {
  return Backend && md_contains(Backend, reinterpret_cast<uintptr_t>(Address));
}
extern "C" int md_jemalloc_hooks(void *Pointer) { return Pointer == &Hooks; }

static void *allocate(extent_hooks_t *, void *NewAddress, size_t Size,
                       size_t Alignment, bool *Zero, bool *Commit, unsigned Arena) {
  if (!PayloadRequest)
    return DefaultHooks->alloc(DefaultHooks, NewAddress, Size, Alignment,
                                Zero, Commit, Arena);
  if (NewAddress) return nullptr;
  md_lease Lease{};
  if (md_acquire_flags(Backend, Size, Alignment, md_process_id(),
                       *Zero ? MD_ZERO : 0, &Lease)) return nullptr;
  LeaseByPage[(Lease.address - kDelegationBase) / PageSize] = Lease;
  *Commit = true;
  return reinterpret_cast<void *>(Lease.address);
}

static bool deallocate(extent_hooks_t *, void *Address, size_t Size,
                        bool Committed, unsigned Arena) {
  if (!md_jemalloc_owns(Address))
    return DefaultHooks->dalloc(DefaultHooks, Address, Size, Committed, Arena);
  size_t Page = (reinterpret_cast<uintptr_t>(Address) - kDelegationBase) / PageSize;
  md_lease Lease = LeaseByPage[Page];
  // Exclusive extent ownership protects this entry. Clear before publication.
  LeaseByPage[Page] = {};
  if (Lease.address != reinterpret_cast<uintptr_t>(Address) || Lease.size != Size ||
      Lease.owner != md_process_id() ||
      md_release(Backend, &Lease, 0, nullptr)) std::abort();
  return false;
}
static void destroy(extent_hooks_t *Hook, void *Address, size_t Size,
                     bool Committed, unsigned Arena) {
  if (!md_jemalloc_owns(Address)) {
    DefaultHooks->destroy(DefaultHooks, Address, Size, Committed, Arena);
    return;
  }
  deallocate(Hook, Address, Size, Committed, Arena);
}
static bool commit(extent_hooks_t *, void *Address, size_t Size, size_t Offset,
                    size_t Length, unsigned Arena) {
  if (!md_jemalloc_owns(Address))
    return DefaultHooks->commit(DefaultHooks, Address, Size, Offset, Length, Arena);
  return false;
}
static bool decommit(extent_hooks_t *, void *Address, size_t Size, size_t Offset,
                      size_t Length, unsigned Arena) {
  return md_jemalloc_owns(Address) || !DefaultHooks->decommit ||
      DefaultHooks->decommit(DefaultHooks, Address, Size, Offset, Length, Arena);
}
static bool purgeLazy(extent_hooks_t *, void *Address, size_t Size, size_t Offset,
                       size_t Length, unsigned Arena) {
  return md_jemalloc_owns(Address) || !DefaultHooks->purge_lazy ||
      DefaultHooks->purge_lazy(DefaultHooks, Address, Size, Offset, Length, Arena);
}
static bool purgeForced(extent_hooks_t *, void *Address, size_t Size, size_t Offset,
                         size_t Length, unsigned Arena) {
  return md_jemalloc_owns(Address) || !DefaultHooks->purge_forced ||
      DefaultHooks->purge_forced(DefaultHooks, Address, Size, Offset, Length, Arena);
}
static bool split(extent_hooks_t *, void *Address, size_t Size, size_t First,
                   size_t Second, bool Committed, unsigned Arena) {
  if (md_jemalloc_owns(Address)) return true;
  return DefaultHooks->split(DefaultHooks, Address, Size, First, Second,
                              Committed, Arena);
}
static bool merge(extent_hooks_t *, void *First, size_t FirstSize, void *Second,
                   size_t SecondSize, bool Committed, unsigned Arena) {
  if (md_jemalloc_owns(First) || md_jemalloc_owns(Second)) return true;
  return DefaultHooks->merge(DefaultHooks, First, FirstSize, Second, SecondSize,
                              Committed, Arena);
}

extern "C" int md_jemalloc_init(const char *Domain, unsigned *Arena) {
  size_t HookSize = sizeof(DefaultHooks);
  int Result = je_mallctl("arena.0.extent_hooks", &DefaultHooks, &HookSize,
                          nullptr, 0);
  if (Result) return Result;
  PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  md_config Config{Domain, kDelegationBase, MD_DEFAULT_ARENA_CAPACITY, 0};
  Result = md_backend_open(&Backend, &Config);
  if (Result) return Result;
  // Derive the lookup window from the backend layout, including guard holes.
  md_arena_layout Last{};
  Result = md_get_arena_layout(Backend, md_arena_count(Backend) - 1, &Last);
  if (Result) return Result;
  size_t Entries = (Last.data_base - kDelegationBase) / PageSize + Last.nr_pages;
  LeaseByPage = static_cast<md_lease *>(mmap(nullptr, Entries * sizeof(md_lease),
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (LeaseByPage == MAP_FAILED) return ENOMEM;
  Hooks = {allocate, deallocate, destroy, commit, decommit, purgeLazy, purgeForced,
            split, merge};
  extent_hooks_t *Pointer = &Hooks;
  size_t Size = sizeof(*Arena);
  return je_mallctl("arenas.create", Arena, &Size, &Pointer, sizeof(Pointer));
}
extern "C" void md_jemalloc_stats(md_stats *Stats) { md_get_stats(Backend, Stats); }

extern "C" int md_jemalloc_checkpoint() {
  return Backend ? md_backend_checkpoint(Backend) : ENODEV;
}
