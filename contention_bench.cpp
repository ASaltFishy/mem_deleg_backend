#include "delegation_backend.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <thread>
#include <vector>
#include <unistd.h>

static void pin(int Cpu) {
  cpu_set_t Set; CPU_ZERO(&Set); CPU_SET(Cpu, &Set);
  assert(sched_setaffinity(0, sizeof(Set), &Set) == 0);
}
int main(int Argc, char **Argv) {
  assert(Argc >= 2);
  int Count = Argc - 1;
  char Domain[96]; snprintf(Domain, sizeof(Domain), "md_contention_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend; assert(md_backend_open(&Backend, &Config) == 0);
  std::atomic<int> Ready{0}; std::atomic<bool> Start{false};
  constexpr int Iterations = 100000;
  std::vector<std::thread> Workers;
  for (int W = 0; W < Count; ++W) Workers.emplace_back([&, W] {
    pin(atoi(Argv[W + 1]));
    for (int I = 0; I < 1000; ++I) {
      md_lease L; assert(md_acquire(Backend, 4096, 4096, 1001, &L) == 0);
      assert(md_release(Backend, &L, 0, nullptr) == 0);
    }
    Ready.fetch_add(1);
    while (!Start.load()) std::this_thread::yield();
    for (int I = 0; I < Iterations; ++I) {
      md_lease L; assert(md_acquire(Backend, 4096, 4096, 1001, &L) == 0);
      assert(md_release(Backend, &L, 0, nullptr) == 0);
    }
  });
  while (Ready.load() != Count) std::this_thread::yield();
  auto Begin = std::chrono::steady_clock::now(); Start.store(true);
  for (auto &W : Workers) W.join();
  double Ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - Begin).count();
  md_stats S{}; md_get_stats(Backend, &S); assert(S.bytes_in_use == 0);
  printf("threads=%d wall_ns_per_pair=%.2f pairs_per_sec=%.0f locks=%llu contended=%llu wait_ns=%llu rseq=%llu aborts=%llu remote=%llu\n",
      Count, Ns / (Iterations * Count), (Iterations * Count) * 1e9 / Ns,
      (unsigned long long)S.lock_acquisitions, (unsigned long long)S.lock_contentions,
      (unsigned long long)S.lock_wait_ns, (unsigned long long)S.rseq_successes,
      (unsigned long long)S.rseq_aborts, (unsigned long long)S.remote_enqueued);
  md_backend_close(Backend);
  char Path[160]; snprintf(Path, sizeof(Path), "/dev/shm/%s_meta", Domain); unlink(Path);
  snprintf(Path, sizeof(Path), "/dev/shm/%s_data", Domain); unlink(Path);
}
