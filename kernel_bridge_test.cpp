#include "kernel_bridge.h"

#include <cassert>
#include <cstdio>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int main() {
#if defined(SYS_memfd_create)
  const long PageSize = sysconf(_SC_PAGESIZE);
  assert(PageSize > 0);
  const uint32_t Pages = 16;
  const int Fd = static_cast<int>(syscall(SYS_memfd_create,
                                           "md-kernel-bridge-test", 0));
  assert(Fd >= 0);
  assert(ftruncate(Fd, PageSize * Pages) == 0);
  void *Mapping = mmap(nullptr, PageSize * Pages, PROT_READ | PROT_WRITE,
                       MAP_SHARED, Fd, 0);
  assert(Mapping != MAP_FAILED);

  md_kernel_bridge_config Config{};
  Config.arena_cpu = 0;
  Config.arena_data_base = reinterpret_cast<uintptr_t>(Mapping);
  Config.arena_nr_pages = Pages;
  Config.mode = MD_KERNEL_BRIDGE_OPTIONAL;
  md_kernel_bridge *Bridge = nullptr;
  assert(md_kernel_bridge_open(&Bridge, &Config) == 0);
  if (Bridge) {
    assert(md_kernel_bridge_transition(Bridge, MD_KERNEL_LOG_ALLOC, 0, 1,
                                       MD_KERNEL_LOG_PREFAULT) == 0);
    assert(md_kernel_bridge_transition(Bridge, MD_KERNEL_LOG_FREE, 0, 1, 0) ==
           0);
    md_kernel_bridge_close(Bridge);
    puts("kernel_bridge_test: PASS active-kernel path");
  } else {
    puts("kernel_bridge_test: PASS optional unsupported-kernel fallback");
  }
  munmap(Mapping, PageSize * Pages);
  close(Fd);
#else
  puts("kernel_bridge_test: SKIP memfd_create unavailable");
#endif
  return 0;
}
