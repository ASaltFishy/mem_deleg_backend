#include "delegation_backend.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <thread>
#include <vector>
#include <unistd.h>
int main(int Argc, char **Argv) {
  assert(Argc == 2); int Cpu = atoi(Argv[1]);
  char Domain[96]; snprintf(Domain, sizeof(Domain), "md_preempt_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend; assert(md_backend_open(&Backend, &Config) == 0);
  std::atomic<int> Ready{0}; std::vector<std::thread> Workers;
  for (int W = 0; W < 4; ++W) Workers.emplace_back([&, W] {
    cpu_set_t Set; CPU_ZERO(&Set); CPU_SET(Cpu, &Set);
    assert(sched_setaffinity(0, sizeof(Set), &Set) == 0);
    Ready.fetch_add(1);
    while (Ready.load() != 4) sched_yield();
    for (int I = 0; I < 200; ++I) {
      md_lease L{}; assert(md_acquire(Backend, 4096, 4096, 1001 + W, &L) == 0);
      memset(reinterpret_cast<void *>(L.address), W + 1, L.size);
      sched_yield();
      for (size_t Offset = 0; Offset < L.size; ++Offset)
        assert(reinterpret_cast<unsigned char *>(L.address)[Offset] == W + 1);
      assert(md_release(Backend, &L, 0, nullptr) == 0);
    }
  });
  for (auto &W : Workers) W.join();
  md_stats Stats{}; md_get_stats(Backend, &Stats);
  assert(Stats.bytes_in_use == 0 && Stats.lock_contentions > 0);
  printf("preemption_test: PASS same_cpu=%d contended=%llu wait_ns=%llu\n", Cpu,
      (unsigned long long)Stats.lock_contentions, (unsigned long long)Stats.lock_wait_ns);
  md_backend_close(Backend);
  char Path[160]; snprintf(Path, sizeof(Path), "/dev/shm/%s_meta", Domain); unlink(Path);
  snprintf(Path, sizeof(Path), "/dev/shm/%s_data", Domain); unlink(Path);
}
