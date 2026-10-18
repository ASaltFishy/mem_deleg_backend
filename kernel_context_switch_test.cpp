#include "include/memory_delegation_prctl.h"
#include "include/uapi/linux/memory_delegation.h"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void writeByte(int Fd, char Value) {
  assert(write(Fd, &Value, 1) == 1);
}

char readByte(int Fd) {
  char Value = 0;
  assert(read(Fd, &Value, 1) == 1);
  return Value;
}

void pinToCpu(unsigned Cpu) {
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(Cpu, &Set);
  assert(sched_setaffinity(0, sizeof(Set), &Set) == 0);
  assert(sched_getcpu() == static_cast<int>(Cpu));
}

md_log_ring *makeRing(size_t PageSize, uint32_t ArenaPages) {
  void *Memory = mmap(nullptr, PageSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(Memory != MAP_FAILED);
  std::memset(Memory, 0, PageSize);
  auto *Ring = static_cast<md_log_ring *>(Memory);
  Ring->magic = MD_LOG_RING_MAGIC;
  Ring->version = MD_LOG_RING_VERSION;
  Ring->capacity = static_cast<uint32_t>(
      (PageSize - sizeof(*Ring)) / sizeof(md_shadow_log));
  Ring->arena_nr_pages = ArenaPages;
  Ring->ring_size = static_cast<uint32_t>(PageSize);
  return Ring;
}

void registerRing(md_log_ring *Ring, unsigned Cpu, uintptr_t Base,
                  bool Reset) {
  unsigned long Flags = PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND |
      (Reset ? PR_MEMORY_DELEGATION_F_RESET_ARENA : 0UL);
  assert(prctl(PR_SET_MEMORY_DELEGATION_LOG,
               reinterpret_cast<unsigned long>(Ring), Flags,
               static_cast<unsigned long>(Cpu),
               static_cast<unsigned long>(Base)) == 0);
}

void appendOnly(md_log_ring *Ring, unsigned Cpu, unsigned Op,
                uint32_t StartPage, uint32_t Pages) {
  uint32_t Head = __atomic_load_n(&Ring->head, __ATOMIC_ACQUIRE);
  uint32_t Tail = __atomic_load_n(&Ring->tail, __ATOMIC_RELAXED);
  assert(Tail - Head < Ring->capacity);
  md_shadow_log &Entry = Ring->entries[Tail % Ring->capacity];
  Entry.op = static_cast<uint8_t>(Op);
  Entry.src_cpu = static_cast<uint8_t>(Cpu);
  Entry.flags = 0;
  Entry.start_page = StartPage;
  Entry.nr_pages = Pages;
  __atomic_store_n(&Ring->last_error, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&Ring->tail, Tail + 1, __ATOMIC_RELEASE);
}

bool probeReadable(volatile unsigned char *Address) {
  pid_t Probe = fork();
  assert(Probe >= 0);
  if (Probe == 0) {
    volatile unsigned char Value = *Address;
    (void)Value;
    _exit(0);
  }
  int Status = 0;
  assert(waitpid(Probe, &Status, 0) == Probe);
  if (WIFEXITED(Status))
    return WEXITSTATUS(Status) == 0;
  assert(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGSEGV);
  return false;
}

}  // namespace

int main() {
#if !defined(SYS_memfd_create)
  std::puts("kernel_context_switch_test: SKIP memfd_create unavailable");
  return 0;
#else
  constexpr unsigned Cpu = 0;
  constexpr uint32_t ArenaPages = 16;
  pinToCpu(Cpu);
  const size_t PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  assert(PageSize > 0);

  int Fd = static_cast<int>(
      syscall(SYS_memfd_create, "md-context-switch-test", 0));
  assert(Fd >= 0);
  assert(ftruncate(Fd, PageSize * ArenaPages) == 0);
  void *Mapping = mmap(nullptr, PageSize * ArenaPages,
                       PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  assert(Mapping != MAP_FAILED);
  auto *Bytes = static_cast<volatile unsigned char *>(Mapping);

  md_log_ring *ParentRing = makeRing(PageSize, ArenaPages);
  registerRing(ParentRing, Cpu, reinterpret_cast<uintptr_t>(Mapping), true);

  int ParentToChild[2];
  int ChildToParent[2];
  assert(pipe(ParentToChild) == 0);
  assert(pipe(ChildToParent) == 0);
  pid_t Child = fork();
  assert(Child >= 0);
  if (Child == 0) {
    pinToCpu(Cpu);
    md_log_ring *ChildRing = makeRing(PageSize, ArenaPages);
    registerRing(ChildRing, Cpu, reinterpret_cast<uintptr_t>(Mapping), false);

    // Both processes populate PTEs while both pages are still globally free.
    (void)Bytes[0];
    (void)Bytes[PageSize];
    writeByte(ChildToParent[1], 'R');

    assert(readByte(ParentToChild[0]) == 'A');
    // Parent's switch-out must drain its ALLOC and the switch-in task work
    // must revoke this child's already-present PTE for page 0.
    if (probeReadable(Bytes))
      _exit(20);

    appendOnly(ChildRing, Cpu, MD_LOG_ALLOC, 1, 1);
    writeByte(ChildToParent[1], 'B');
    assert(readByte(ParentToChild[0]) == 'F');

    // Parent's FREE is also consumed only by switching away. Page 0 should
    // become faultable again for this non-owner.
    if (!probeReadable(Bytes))
      _exit(21);
    appendOnly(ChildRing, Cpu, MD_LOG_FREE, 1, 1);
    writeByte(ChildToParent[1], 'D');
    _exit(0);
  }

  assert(readByte(ChildToParent[0]) == 'R');
  (void)Bytes[0];
  (void)Bytes[PageSize];

  appendOnly(ParentRing, Cpu, MD_LOG_ALLOC, 0, 1);
  writeByte(ParentToChild[1], 'A');
  assert(readByte(ChildToParent[0]) == 'B');

  // The child owns page 1 after its switch-out. Parent retains page 0 but
  // must lose its stale page-1 PTE.
  assert(Bytes[0] == 0);
  assert(!probeReadable(Bytes + PageSize));

  appendOnly(ParentRing, Cpu, MD_LOG_FREE, 0, 1);
  writeByte(ParentToChild[1], 'F');
  assert(readByte(ChildToParent[0]) == 'D');

  int Status = 0;
  assert(waitpid(Child, &Status, 0) == Child);
  assert(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  assert(__atomic_load_n(&ParentRing->head, __ATOMIC_ACQUIRE) ==
         __atomic_load_n(&ParentRing->tail, __ATOMIC_ACQUIRE));
  assert(__atomic_load_n(&ParentRing->last_error, __ATOMIC_ACQUIRE) == 0);

  std::puts("kernel_context_switch_test: PASS async drain, live-process ownership, stale-PTE revoke, owner access, free refault");
  return 0;
#endif
}
