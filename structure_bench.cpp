#include "delegation_backend.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>
#include <unistd.h>
using Clock = std::chrono::steady_clock;
int main() {
  char Domain[96]; snprintf(Domain, sizeof(Domain), "md_structure_%d", getpid());
  md_config Config{Domain, 0x310000000000ULL, 64ULL << 20, 0};
  md_backend *Backend; assert(md_backend_open(&Backend, &Config) == 0);
  constexpr int Count = 4096;
  std::vector<md_lease> Leases(Count);
  auto Begin = Clock::now();
  for (auto &Lease : Leases) assert(md_acquire(Backend, 4096, 4096, 1001, &Lease) == 0);
  double Alloc = std::chrono::duration<double, std::nano>(Clock::now() - Begin).count() / Count;
  Begin = Clock::now();
  for (int I = 0; I < Count; I += 2) assert(md_release(Backend, &Leases[I], 0, nullptr) == 0);
  double Free = std::chrono::duration<double, std::nano>(Clock::now() - Begin).count() / (Count / 2);
  for (int I = Count - 1; I >= 1; I -= 2) assert(md_release(Backend, &Leases[I], 0, nullptr) == 0);
  md_stats S{}; md_get_stats(Backend, &S); assert(S.bytes_in_use == 0);
  printf("bump_alloc_ns=%.2f ascending_fragmented_free_ns=%.2f descriptor_pops=%llu hint_hits=%llu address_steps=%llu\n",
      Alloc, Free, (unsigned long long)S.descriptor_pops,
      (unsigned long long)S.hint_hits, (unsigned long long)S.address_steps);
  md_backend_close(Backend);
  char Path[160]; snprintf(Path, sizeof(Path), "/dev/shm/%s_meta", Domain); unlink(Path);
  snprintf(Path, sizeof(Path), "/dev/shm/%s_data", Domain); unlink(Path);
}
