#include "include/memory_delegation_prctl.h"
#include "include/uapi/linux/memory_delegation.h"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr unsigned Cpu = 0;
constexpr uint32_t ArenaPages = 16;
constexpr uintptr_t ArenaBase = 0x7000000000ULL;

void sendByte(int Fd, char Value) {
  assert(send(Fd, &Value, 1, 0) == 1);
}

char recvByte(int Fd) {
  char Value = 0;
  assert(recv(Fd, &Value, 1, MSG_WAITALL) == 1);
  return Value;
}

void sendFd(int Socket, int Fd) {
  char Byte = 'D';
  iovec Io{&Byte, 1};
  alignas(cmsghdr) char Control[CMSG_SPACE(sizeof(int))] = {};
  msghdr Message{};
  Message.msg_iov = &Io;
  Message.msg_iovlen = 1;
  Message.msg_control = Control;
  Message.msg_controllen = sizeof(Control);
  cmsghdr *Header = CMSG_FIRSTHDR(&Message);
  assert(Header != nullptr);
  Header->cmsg_level = SOL_SOCKET;
  Header->cmsg_type = SCM_RIGHTS;
  Header->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(Header), &Fd, sizeof(Fd));
  assert(sendmsg(Socket, &Message, 0) == 1);
}

int recvFd(int Socket) {
  char Byte = 0;
  iovec Io{&Byte, 1};
  alignas(cmsghdr) char Control[CMSG_SPACE(sizeof(int))] = {};
  msghdr Message{};
  Message.msg_iov = &Io;
  Message.msg_iovlen = 1;
  Message.msg_control = Control;
  Message.msg_controllen = sizeof(Control);
  assert(recvmsg(Socket, &Message, 0) == 1);
  assert(Byte == 'D');
  cmsghdr *Header = CMSG_FIRSTHDR(&Message);
  assert(Header != nullptr && Header->cmsg_level == SOL_SOCKET &&
         Header->cmsg_type == SCM_RIGHTS &&
         Header->cmsg_len == CMSG_LEN(sizeof(int)));
  int Fd = -1;
  std::memcpy(&Fd, CMSG_DATA(Header), sizeof(Fd));
  assert(Fd >= 0);
  return Fd;
}

void pinToCpu() {
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(Cpu, &Set);
  assert(sched_setaffinity(0, sizeof(Set), &Set) == 0);
  assert(sched_getcpu() == static_cast<int>(Cpu));
}

md_log_ring *makeRing(size_t PageSize) {
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

void registerRing(md_log_ring *Ring, bool Reset) {
  unsigned long Flags = PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND |
      (Reset ? PR_MEMORY_DELEGATION_F_RESET_ARENA : 0UL);
  assert(prctl(PR_SET_MEMORY_DELEGATION_LOG,
               reinterpret_cast<unsigned long>(Ring), Flags, Cpu,
               ArenaBase) == 0);
}

void appendOnly(md_log_ring *Ring, unsigned Op, uint32_t StartPage) {
  uint32_t Head = __atomic_load_n(&Ring->head, __ATOMIC_ACQUIRE);
  uint32_t Tail = __atomic_load_n(&Ring->tail, __ATOMIC_RELAXED);
  assert(Tail - Head < Ring->capacity);
  md_shadow_log &Entry = Ring->entries[Tail % Ring->capacity];
  Entry.op = static_cast<uint8_t>(Op);
  Entry.src_cpu = static_cast<uint8_t>(Cpu);
  Entry.flags = 0;
  Entry.start_page = StartPage;
  Entry.nr_pages = 1;
  __atomic_store_n(&Ring->last_error, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&Ring->tail, Tail + 1, __ATOMIC_RELEASE);
}

sigjmp_buf FaultJump;
volatile sig_atomic_t Probing = 0;

void faultHandler(int Signal) {
  if (Probing)
    siglongjmp(FaultJump, Signal);
  _exit(128 + Signal);
}

bool readableInCurrentProcess(volatile unsigned char *Address) {
  struct sigaction Action{};
  struct sigaction OldSegv{};
  struct sigaction OldBus{};
  Action.sa_handler = faultHandler;
  sigemptyset(&Action.sa_mask);
  assert(sigaction(SIGSEGV, &Action, &OldSegv) == 0);
  assert(sigaction(SIGBUS, &Action, &OldBus) == 0);

  Probing = 1;
  volatile sig_atomic_t Fault = sigsetjmp(FaultJump, 1);
  if (Fault == 0) {
    volatile unsigned char Value = *Address;
    (void)Value;
  }
  Probing = 0;
  assert(sigaction(SIGSEGV, &OldSegv, nullptr) == 0);
  assert(sigaction(SIGBUS, &OldBus, nullptr) == 0);
  return Fault == 0;
}

int worker(char Role, int Control) {
  pinToCpu();
  const size_t PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  assert(PageSize > 0);
  int ArenaFd = recvFd(Control);
  void *Mapping = mmap(reinterpret_cast<void *>(ArenaBase),
                       PageSize * ArenaPages, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, ArenaFd, 0);
  assert(Mapping == reinterpret_cast<void *>(ArenaBase));
  close(ArenaFd);
  auto *Bytes = static_cast<volatile unsigned char *>(Mapping);
  md_log_ring *Ring = makeRing(PageSize);

  if (Role == 'A') {
    registerRing(Ring, true);
    sendByte(Control, 'G');
  } else {
    sendByte(Control, 'M');
    assert(recvByte(Control) == 'G');
    registerRing(Ring, false);
    sendByte(Control, 'G');
  }

  assert(recvByte(Control) == 'P');
  (void)Bytes[0];
  (void)Bytes[PageSize];
  sendByte(Control, 'R');

  if (Role == 'A') {
    assert(recvByte(Control) == 'L');
    appendOnly(Ring, MD_LOG_ALLOC, 0);
    sendByte(Control, 'A');

    assert(recvByte(Control) == 'C');
    assert(readableInCurrentProcess(Bytes));
    assert(!readableInCurrentProcess(Bytes + PageSize));
    sendByte(Control, 'C');

    assert(recvByte(Control) == 'F');
    appendOnly(Ring, MD_LOG_FREE, 0);
    sendByte(Control, 'F');
  } else {
    assert(recvByte(Control) == 'C');
    assert(!readableInCurrentProcess(Bytes));
    sendByte(Control, 'C');

    assert(recvByte(Control) == 'L');
    appendOnly(Ring, MD_LOG_ALLOC, 1);
    sendByte(Control, 'B');

    assert(recvByte(Control) == 'U');
    assert(readableInCurrentProcess(Bytes));
    appendOnly(Ring, MD_LOG_FREE, 1);
    sendByte(Control, 'U');
  }

  assert(recvByte(Control) == 'Q');
  assert(__atomic_load_n(&Ring->head, __ATOMIC_ACQUIRE) ==
         __atomic_load_n(&Ring->tail, __ATOMIC_ACQUIRE));
  assert(__atomic_load_n(&Ring->last_error, __ATOMIC_ACQUIRE) == 0);
  return 0;
}

pid_t spawnWorker(const char *Self, char Role, int Pair[2], int Other[2]) {
  pid_t Pid = fork();
  assert(Pid >= 0);
  if (Pid != 0)
    return Pid;
  close(Pair[0]);
  close(Other[0]);
  close(Other[1]);
  char FdText[32];
  std::snprintf(FdText, sizeof(FdText), "%d", Pair[1]);
  char RoleText[2] = {Role, '\0'};
  execl(Self, Self, "--worker", RoleText, FdText, nullptr);
  _exit(127);
}

}  // namespace

int main(int Argc, char **Argv) {
  if (Argc == 4 && std::strcmp(Argv[1], "--worker") == 0)
    return worker(Argv[2][0], std::atoi(Argv[3]));

#if !defined(SYS_memfd_create)
  std::puts("kernel_unrelated_process_test: SKIP memfd_create unavailable");
  return 0;
#else
  int A[2];
  int B[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, A) == 0);
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, B) == 0);
  pid_t APid = spawnWorker(Argv[0], 'A', A, B);
  pid_t BPid = spawnWorker(Argv[0], 'B', B, A);
  close(A[1]);
  close(B[1]);

  const size_t PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  int ArenaFd = static_cast<int>(
      syscall(SYS_memfd_create, "md-unrelated-process-test", 0));
  assert(ArenaFd >= 0);
  assert(ftruncate(ArenaFd, PageSize * ArenaPages) == 0);
  sendFd(A[0], ArenaFd);
  sendFd(B[0], ArenaFd);
  close(ArenaFd);

  assert(recvByte(B[0]) == 'M');
  assert(recvByte(A[0]) == 'G');
  sendByte(B[0], 'G');
  assert(recvByte(B[0]) == 'G');

  sendByte(A[0], 'P');
  sendByte(B[0], 'P');
  assert(recvByte(A[0]) == 'R');
  assert(recvByte(B[0]) == 'R');

  sendByte(A[0], 'L');
  assert(recvByte(A[0]) == 'A');
  sendByte(B[0], 'C');
  assert(recvByte(B[0]) == 'C');

  sendByte(B[0], 'L');
  assert(recvByte(B[0]) == 'B');
  sendByte(A[0], 'C');
  assert(recvByte(A[0]) == 'C');

  sendByte(A[0], 'F');
  assert(recvByte(A[0]) == 'F');
  sendByte(B[0], 'U');
  assert(recvByte(B[0]) == 'U');

  sendByte(A[0], 'Q');
  sendByte(B[0], 'Q');
  int AStatus = 0;
  int BStatus = 0;
  assert(waitpid(APid, &AStatus, 0) == APid);
  assert(waitpid(BPid, &BStatus, 0) == BPid);
  assert(WIFEXITED(AStatus) && WEXITSTATUS(AStatus) == 0);
  assert(WIFEXITED(BStatus) && WEXITSTATUS(BStatus) == 0);

  std::puts("kernel_unrelated_process_test: PASS independent exec workers, shared backing fd, async drain, bidirectional isolation, free refault");
  return 0;
#endif
}
