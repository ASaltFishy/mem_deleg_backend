#include "kernel_bridge.h"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
#if !defined(SYS_memfd_create)
  std::puts("kernel_external_backend_test: SKIP memfd_create unavailable");
  return 0;
#else
  const long PageSize = sysconf(_SC_PAGESIZE);
  assert(PageSize > 0);
  constexpr uint32_t Pages = 16;
  const int Fd = static_cast<int>(
      syscall(SYS_memfd_create, "md-external-backend-test", 0));
  assert(Fd >= 0);
  assert(ftruncate(Fd, PageSize * Pages) == 0);
  void *Mapping = mmap(nullptr, PageSize * Pages, PROT_READ | PROT_WRITE,
                       MAP_SHARED, Fd, 0);
  assert(Mapping != MAP_FAILED);

  md_kernel_bridge_config Config{};
  Config.arena_cpu = 0;
  Config.arena_data_base = reinterpret_cast<uintptr_t>(Mapping);
  Config.arena_nr_pages = Pages;
  Config.reset_arena = 1;
  Config.mode = MD_KERNEL_BRIDGE_REQUIRED;
  md_kernel_bridge *ParentBridge = nullptr;
  assert(md_kernel_bridge_open(&ParentBridge, &Config) == 0);
  assert(ParentBridge != nullptr);

  pid_t Owner = fork();
  assert(Owner >= 0);
  if (Owner == 0) {
    Config.reset_arena = 0;
    md_kernel_bridge *ChildBridge = nullptr;
    if (md_kernel_bridge_open(&ChildBridge, &Config) != 0 || !ChildBridge)
      _exit(10);
    if (md_kernel_bridge_transition(ChildBridge, MD_KERNEL_LOG_ALLOC,
                                    0, 1, 0) != 0)
      _exit(11);
    *static_cast<volatile unsigned char *>(Mapping) = 0x5a;
    _exit(0);  // Deliberately die without FREE.
  }

  int Status = 0;
  assert(waitpid(Owner, &Status, 0) == Owner);
  assert(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);

  // The dead owner's page must remain quarantined, and the rejected ALLOC
  // must be reported to userspace instead of being silently dropped.
  assert(md_kernel_bridge_transition(ParentBridge, MD_KERNEL_LOG_ALLOC,
                                     0, 1, 0) == EBUSY);

  pid_t Probe = fork();
  assert(Probe >= 0);
  if (Probe == 0) {
    volatile unsigned char Value =
        *static_cast<volatile unsigned char *>(Mapping);
    (void)Value;
    _exit(0);
  }
  assert(waitpid(Probe, &Status, 0) == Probe);
  assert(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGSEGV);

  assert(md_kernel_bridge_transition(ParentBridge, MD_KERNEL_LOG_ALLOC,
                                     1, 1, 0) == 0);
  static_cast<volatile unsigned char *>(Mapping)[PageSize] = 0xa5;
  assert(md_kernel_bridge_transition(ParentBridge, MD_KERNEL_LOG_FREE,
                                     1, 1, 0) == 0);

  md_kernel_bridge_close(ParentBridge);
  munmap(Mapping, PageSize * Pages);
  close(Fd);
  std::puts("kernel_external_backend_test: PASS normal transition, rejected-error propagation, owner-death quarantine, fault denial");
  return 0;
#endif
}
