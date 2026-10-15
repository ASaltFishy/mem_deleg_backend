#include "delegation_backend.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static void transfer(int Fd, void *Data, size_t Size, bool Write) {
  char *Bytes = static_cast<char *>(Data);
  while (Size) {
    ssize_t Count = Write ? write(Fd, Bytes, Size) : read(Fd, Bytes, Size);
    if (Count < 0 && errno == EINTR) continue;
    assert(Count > 0);
    Bytes += Count;
    Size -= Count;
  }
}

static unsigned fdCount() {
  DIR *Directory = opendir("/proc/self/fd");
  assert(Directory);
  unsigned Count = 0;
  while (readdir(Directory)) ++Count;
  closedir(Directory);
  return Count;
}

static int worker(const char *Domain, int Cpu, int CreatorGate) {
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(Cpu, &Set);
  assert(sched_setaffinity(0, sizeof(Set), &Set) == 0);
  if (CreatorGate >= 0) {
    char Fd[32];
    snprintf(Fd, sizeof(Fd), "%d", CreatorGate);
    setenv("MD_TEST_CREATOR_GATE_FD", Fd, 1);
  } else unsetenv("MD_TEST_CREATOR_GATE_FD");
  md_config Config{Domain, 0x280000000000ULL, 16ULL << 20, 0};
  md_backend *Backend = nullptr;
  int Error = md_backend_open(&Backend, &Config);
  if (Error) return Error;
  md_lease Lease{};
  Error = md_acquire(Backend, 4096, 4096, md_process_id(), &Lease);
  if (Error) return Error;
  memset(reinterpret_cast<void *>(Lease.address), 0x5a, Lease.size);
  assert(md_release(Backend, &Lease, 0, nullptr) == 0);

  // Both validation and fixed-address mapping failures must close descriptors
  // and unmap temporary metadata without harming the existing live domain.
  unsigned Before = fdCount();
  for (unsigned I = 0; I < 32; ++I) {
    md_backend *Other = nullptr;
    md_config Bad = Config;
    Bad.capacity *= 2;
    assert(md_backend_open(&Other, &Bad) == EINVAL && Other == nullptr);
    assert(md_backend_open(&Other, &Config) == EEXIST && Other == nullptr);
    Bad = Config;
    Bad.reset = 1;
    assert(md_backend_open(&Other, &Bad) == EINVAL && Other == nullptr);
  }
  assert(fdCount() == Before);
  assert(md_acquire(Backend, 4096, 4096, md_process_id(), &Lease) == 0);
  assert(md_release(Backend, &Lease, 0, nullptr) == 0);
  md_backend_close(Backend);
  assert(md_backend_open(&Backend, &Config) == 0);
  md_backend_close(Backend);
  return 0;
}

int main() {
  alarm(180);
  setenv("MD_SINGLE_ARENA", "0", 1);
  cpu_set_t Allowed;
  assert(sched_getaffinity(0, sizeof(Allowed), &Allowed) == 0);
  std::vector<int> Cpus;
  for (int Cpu = 0; Cpu < CPU_SETSIZE && Cpus.size() < 8; ++Cpu)
    if (CPU_ISSET(Cpu, &Allowed)) Cpus.push_back(Cpu);
  assert(!Cpus.empty());
  constexpr unsigned Workers = 8;
  constexpr unsigned Rounds = 24;
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    bool Delay = Round < 4;
    char Domain[100], MetaName[110], DataName[110];
    snprintf(Domain, sizeof(Domain), "md_first_open_%d_%u", getpid(), Round);
    snprintf(MetaName, sizeof(MetaName), "/%s_meta", Domain);
    snprintf(DataName, sizeof(DataName), "/%s_data", Domain);
    int Gate[2], Result[2], CreatorGate[2];
    assert(pipe(Gate) == 0 && pipe(Result) == 0 && pipe(CreatorGate) == 0);
    std::vector<pid_t> Children;
    for (unsigned I = 0; I < Workers; ++I) {
      pid_t Child = fork();
      assert(Child >= 0);
      if (Child == 0) {
        close(Gate[1]);
        close(Result[0]);
        close(CreatorGate[1]);
        char Token;
        if (!(Delay && I == 0)) transfer(Gate[0], &Token, 1, false);
        int Error = worker(Domain, Cpus[I % Cpus.size()],
                           Delay && I == 0 ? CreatorGate[0] : -1);
        transfer(Result[1], &Error, sizeof(Error), true);
        _exit(Error ? 1 : 0);
      }
      Children.push_back(Child);
      if (Delay && I == 0) {
        // Observe the published but empty metadata inode while its creator is
        // deliberately paused before flock. An attacher must initialize it.
        bool Found = false;
        for (unsigned Retry = 0; Retry < 10000; ++Retry) {
          int Fd = shm_open(MetaName, O_RDWR, 0600);
          if (Fd >= 0) {
            struct stat Stat{};
            assert(fstat(Fd, &Stat) == 0 && Stat.st_size == 0);
            close(Fd);
            Found = true;
            break;
          }
          assert(errno == ENOENT);
          usleep(100);
        }
        assert(Found);
      }
    }
    close(Gate[0]);
    close(Result[1]);
    close(CreatorGate[0]);
    for (unsigned I = Delay ? 1 : 0; I < Workers; ++I) {
      char Token = 'G';
      transfer(Gate[1], &Token, 1, true);
    }
    close(Gate[1]);
    unsigned Errors = 0;
    for (unsigned I = 0; I < Workers; ++I) {
      // Only release the paused name creator after all other attachers have
      // completed. This guarantees that a noncreator initializes the domain.
      if (Delay && I == Workers - 1) {
        char Token = 'G';
        transfer(CreatorGate[1], &Token, 1, true);
      }
      int Error;
      transfer(Result[0], &Error, sizeof(Error), false);
      if (Error) {
        fprintf(stderr, "round=%u open/use failed: %s (%d)\n", Round, strerror(Error), Error);
        ++Errors;
      }
    }
    close(CreatorGate[1]);
    close(Result[0]);
    for (pid_t Child : Children) {
      int Status;
      assert(waitpid(Child, &Status, 0) == Child);
      if (!WIFEXITED(Status) || WEXITSTATUS(Status)) ++Errors;
    }
    if (!Errors) {
      md_config Config{Domain, 0x280000000000ULL, 16ULL << 20, 0};
      md_backend *Backend = nullptr;
      assert(md_backend_open(&Backend, &Config) == 0);
      md_stats Stats{};
      md_get_stats(Backend, &Stats);
      assert(Stats.acquire_successes == Workers * 2);
      assert(Stats.release_successes == Workers * 2);
      assert(Stats.bytes_in_use == 0);
      md_backend_close(Backend);
    }
    assert(shm_unlink(MetaName) == 0);
    if (shm_unlink(DataName) != 0) assert(errno == ENOENT);
    assert(Errors == 0);
  }
  printf("first_open_test: PASS rounds=%u workers=%u delayed_creator_rounds=4 "
         "fd_cleanup=PASS reopen=PASS shared_totals=PASS\n", Rounds, Workers);
}
