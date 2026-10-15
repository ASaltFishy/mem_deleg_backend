#include "delegation_backend.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

static void pin(int Cpu) {
  cpu_set_t Set;
  CPU_ZERO(&Set); CPU_SET(Cpu, &Set);
  assert(sched_setaffinity(0, sizeof(Set), &Set) == 0);
}
static void zero(const md_lease &Lease) {
  for (size_t I = 0; I < Lease.size; ++I)
    assert(reinterpret_cast<unsigned char *>(Lease.address)[I] == 0);
}
static void unlinkDomain(const char *Domain) {
  char Path[160];
  snprintf(Path, sizeof(Path), "/dev/shm/%s_meta", Domain); unlink(Path);
  snprintf(Path, sizeof(Path), "/dev/shm/%s_data", Domain); unlink(Path);
}
int main(int Argc, char **Argv) {
  assert(Argc >= 3);
  int First = atoi(Argv[1]), Second = atoi(Argv[2]);
  pin(First);
  char Domain[96]; snprintf(Domain, sizeof(Domain), "md_concurrency_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend;
  assert(md_backend_open(&Backend, &Config) == 0);
  // Exhaust and recycle the descriptor stack without exhausting payload.
  std::vector<md_lease> Leases(8192);
  for (auto &Lease : Leases) assert(md_acquire(Backend, 4096, 4096, 1001, &Lease) == 0);
  md_lease Extra{};
  assert(md_acquire(Backend, 4096, 4096, 1001, &Extra) == ENOSPC);
  for (auto &Lease : Leases) assert(md_release(Backend, &Lease, 0, nullptr) == 0);
  assert(md_acquire(Backend, 32ULL << 20, 4096, 1001, &Extra) == 0);
  assert(md_release(Backend, &Extra, 0, nullptr) == 0);
  // Removing/recycling the hinted node must not corrupt later insertion.
  for (int Round = 0; Round < 4; ++Round) {
    for (int I = 0; I < 512; ++I)
      assert(md_acquire(Backend, 4096, 4096, 1001, &Leases[I]) == 0);
    for (int I = 0; I < 512; I += 2)
      assert(md_release(Backend, &Leases[I], 0, nullptr) == 0);
    for (int I = 511; I >= 1; I -= 2)
      assert(md_release(Backend, &Leases[I], 0, nullptr) == 0);
  }
  assert(md_acquire(Backend, 65536, 4096, 1001, &Extra) == 0);
  memset(reinterpret_cast<void *>(Extra.address), 0xa5, Extra.size);
  pin(Second);
  md_candidate Pending{};
  assert(md_release(Backend, &Extra, 0, &Pending) == 0);
  assert(Pending.block_id == UINT32_MAX);
  assert(md_release(Backend, &Extra, 0, nullptr) == ESTALE);
  assert(md_drain_remote(Backend, static_cast<unsigned>(First)) == 0);
  pin(First);
  md_lease Reused{};
  assert(md_acquire(Backend, 65536, 4096, 2002, &Reused) == 0);
  assert(Reused.address == Extra.address); zero(Reused);
  assert(md_release(Backend, &Extra, 0, nullptr) == ESTALE);
  assert(md_release(Backend, &Reused, 0, nullptr) == 0);

  // Multiple remote producers enqueue while the owner drains and allocates.
  constexpr int Producers = 4, PerProducer = 128;
  for (int I = 0; I < Producers * PerProducer; ++I)
    assert(md_acquire(Backend, 4096, 4096, 3003, &Leases[I]) == 0);
  std::atomic<int> Ready{0}, Done{0};
  std::vector<std::thread> Workers;
  for (int P = 0; P < Producers; ++P) Workers.emplace_back([&, P] {
    pin(Second); Ready.fetch_add(1);
    while (Ready.load() != Producers) std::this_thread::yield();
    for (int I = P * PerProducer; I < (P + 1) * PerProducer; ++I) {
      assert(md_release(Backend, &Leases[I], 0, nullptr) == 0);
      assert(md_release(Backend, &Leases[I], 0, nullptr) == ESTALE);
    }
    Done.fetch_add(1);
  });
  while (Done.load() != Producers) {
    assert(md_acquire(Backend, 8192, 4096, 4004, &Extra) == 0);
    memset(reinterpret_cast<void *>(Extra.address), 0x44, Extra.size);
    assert(md_release(Backend, &Extra, 0, nullptr) == 0);
  }
  for (auto &Worker : Workers) Worker.join();
  // Separate process remote producer, using inherited shared mapping.
  assert(md_acquire(Backend, 65536, 4096, 5005, &Extra) == 0);
  pid_t Child = fork(); assert(Child >= 0);
  if (!Child) { pin(Second); assert(md_release(Backend, &Extra, 0, nullptr) == 0); _exit(0); }
  int Status; assert(waitpid(Child, &Status, 0) == Child && Status == 0);
  assert(md_drain_remote(Backend, static_cast<unsigned>(First)) == 0);
  // Same-CPU oversubscription exercises preempted holders; no busy-wait lock.
  Workers.clear();
  for (int P = 0; P < 4; ++P) Workers.emplace_back([&, P] {
    pin(First);
    for (int I = 0; I < 5000; ++I) {
      md_lease L{};
      assert(md_acquire(Backend, 4096, 4096, 6000 + P, &L) == 0);
      memset(reinterpret_cast<void *>(L.address), P + 1, L.size);
      if (I % 17 == 0) std::this_thread::yield();
      assert(*reinterpret_cast<unsigned char *>(L.address) == P + 1);
      assert(md_release(Backend, &L, 0, nullptr) == 0);
    }
  });
  for (auto &Worker : Workers) Worker.join();
  md_stats Stats{}; md_get_stats(Backend, &Stats);
  assert(Stats.bytes_in_use == 0);
  assert(Stats.remote_enqueued >= 514 && Stats.remote_enqueued == Stats.remote_drained);
  assert(Stats.hint_hits > 0 && Stats.descriptor_pops >= 8192);
  printf("concurrency_test: PASS remote=%llu drained=%llu locks=%llu contended=%llu rseq=%llu aborts=%llu hint=%llu steps=%llu descriptor_pops=%llu\n",
    (unsigned long long)Stats.remote_enqueued, (unsigned long long)Stats.remote_drained,
    (unsigned long long)Stats.lock_acquisitions, (unsigned long long)Stats.lock_contentions,
    (unsigned long long)Stats.rseq_successes, (unsigned long long)Stats.rseq_aborts,
    (unsigned long long)Stats.hint_hits, (unsigned long long)Stats.address_steps,
    (unsigned long long)Stats.descriptor_pops);
  printf("queue_contentions=%llu queue_wait_ns=%llu arena_wait_ns=%llu\n",
    (unsigned long long)Stats.queue_contentions, (unsigned long long)Stats.queue_wait_ns,
    (unsigned long long)Stats.lock_wait_ns);
  md_backend_close(Backend); unlinkDomain(Domain);
}
