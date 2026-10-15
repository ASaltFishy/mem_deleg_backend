#include "delegation_backend.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <vector>

int main() {
  char Domain[100];
  snprintf(Domain, sizeof(Domain), "codex_backend_test_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL,
                   64ULL << 20, 1};
  md_backend *Backend = nullptr;
  assert(md_backend_open(&Backend, &Config) == 0);
  md_arena_layout Layout{};
  assert(md_get_arena_layout(Backend, 0, &Layout) == 0);
  assert(Layout.arena == 0 && Layout.data_base == Config.fixed_address &&
         Layout.data_size == Config.capacity &&
         Layout.nr_pages == Config.capacity / 4096);
  assert(md_get_arena_layout(Backend, md_arena_count(Backend), &Layout) ==
         EINVAL);
  if (md_arena_count(Backend) > 1) {
    md_arena_layout Next{};
    assert(md_get_arena_layout(Backend, 1, &Next) == 0);
    assert(Next.data_base == Layout.data_base + Config.capacity + 4096);
    assert(!md_contains(Backend, Layout.data_base + Config.capacity));
    assert(md_contains(Backend, Next.data_base));
  }
  md_lease First{};
  assert(md_acquire(Backend, 256 << 10, 4096, 1001, &First) == 0);
  std::memset(reinterpret_cast<void *>(First.address), 0x5a, First.size);
  md_candidate Candidate{};
  assert(md_release(Backend, &First, 1, &Candidate) == 0);
  assert(*reinterpret_cast<unsigned char *>(First.address + 4096) == 0);
  md_lease Second{};
  assert(md_try_acquire(Backend, Candidate, 2002, &Second) == 0);
  assert(Second.address == First.address);
  md_lease Stale{};
  assert(md_try_acquire(Backend, Candidate, 1001, &Stale) == EAGAIN);
  assert(md_release(Backend, &First, 0, nullptr) == ESTALE);
  md_candidate RaceCandidate{};
  assert(md_release(Backend, &Second, 0, &RaceCandidate) == 0);

  int Start[2];
  int Result[2];
  assert(pipe(Start) == 0 && pipe(Result) == 0);
  pid_t Children[2];
  for (int Index = 0; Index < 2; ++Index) {
    Children[Index] = fork();
    assert(Children[Index] >= 0);
    if (Children[Index] == 0) {
      char Byte;
      read(Start[0], &Byte, 1);
      md_lease Won{};
      int Status = md_try_acquire(Backend, RaceCandidate, 3000 + Index, &Won);
      write(Result[1], &Status, sizeof(Status));
      _exit(0);
    }
  }
  write(Start[1], "xx", 2);
  int Successes = 0;
  for (int Index = 0; Index < 2; ++Index) {
    int Status;
    read(Result[0], &Status, sizeof(Status));
    Successes += Status == 0;
    waitpid(Children[Index], nullptr, 0);
  }
  assert(Successes == 1);

  md_lease Large{};
  assert(md_acquire(Backend, 1048576, 65536, 1001, &Large) == 0);
  md_candidate OldLarge{};
  assert(md_release(Backend, &Large, 1, &OldLarge) == 0);
  md_lease Small{};
  assert(md_acquire(Backend, 65536, 65536, 1001, &Small) == 0);
  assert(md_try_acquire(Backend, OldLarge, 1001, &Stale) == EAGAIN);
  assert(md_release(Backend, &Small, 1, nullptr) == 0);
  assert(md_try_acquire(Backend, OldLarge, 1001, &Stale) == EAGAIN);

  std::vector<std::thread> Workers;
  for (uint32_t Worker = 0; Worker < 4; ++Worker) {
    Workers.emplace_back([&, Worker] {
      for (int Iteration = 0; Iteration < 2000; ++Iteration) {
        md_lease Lease{};
        while (md_acquire(Backend, 16384, 4096, 4000 + Worker, &Lease))
          std::this_thread::yield();
        memset(reinterpret_cast<void *>(Lease.address), Worker + 1, Lease.size);
        assert(*reinterpret_cast<unsigned char *>(Lease.address + 8192) == Worker + 1);
        assert(md_release(Backend, &Lease, 1, nullptr) == 0);
        assert(md_release(Backend, &Lease, 1, nullptr) == ESTALE);
      }
    });
  }
  for (auto &Worker : Workers) Worker.join();
  md_lease Impossible{};
  assert(md_acquire(Backend, 128ULL << 20, 4096, 1, &Impossible) == EINVAL);
  assert(md_acquire(Backend, 4096, 6000, 1, &Impossible) == EINVAL);

  md_stats Stats{};
  md_get_stats(Backend, &Stats);
  std::printf("backend_test: PASS acquire=%llu release=%llu split=%llu merge=%llu\n",
              static_cast<unsigned long long>(Stats.acquire_successes),
              static_cast<unsigned long long>(Stats.release_successes),
              static_cast<unsigned long long>(Stats.split_count),
              static_cast<unsigned long long>(Stats.merge_count));
  md_backend_close(Backend);
}
