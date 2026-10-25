// Capacity failure is a native-fallback signal, not an invalid request.
// Runs without the custom kernel, in both instrumentation configurations.
#include "delegation_backend.h"
#include "include/perf_config.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#define CHECK(X) do { if (!(X)) { std::fprintf(stderr, "capacity policy check failed line=%d: %s\n", __LINE__, #X); return 1; } } while (0)
int main() {
  setenv("MD_KERNEL_BRIDGE", "off", 1);
  setenv("MD_SINGLE_ARENA", "1", 1);
  unsetenv("MD_BROKER_SOCKET");
  char Domain[80]; std::snprintf(Domain, sizeof(Domain), "md_capacity_policy_%d", getpid());
  md_config Config{Domain, 0x280000000000ULL, 65536, 0};
  md_backend *Backend = nullptr;
  CHECK(md_backend_open(&Backend, &Config) == 0);
  md_lease First{}, Other{};
  uint32_t Owner = md_process_id();
  CHECK(md_acquire(Backend, Config.capacity + 4096, 4096, Owner, &Other) == ENOMEM);
  CHECK(md_acquire(Backend, 4096, 3, Owner, &Other) == EINVAL);
  CHECK(md_acquire(Backend, Config.capacity, 4096, Owner, &First) == 0);
  std::memset(reinterpret_cast<void *>(First.address), 0x5a, First.size);
  // A small request also falls back while another live block fills the Arena.
  CHECK(md_acquire(Backend, 4096, 4096, Owner, &Other) == ENOMEM);
  CHECK(md_release(Backend, &First, 0, nullptr) == 0);
  CHECK(md_acquire_flags(Backend, Config.capacity, 4096, Owner, MD_ZERO, &Other) == 0);
  for (size_t I=0; I<Other.size; ++I)
    CHECK(reinterpret_cast<unsigned char *>(Other.address)[I] == 0);
  CHECK(md_release(Backend, &Other, 0, nullptr) == 0);
  md_stats Stats{}; md_get_stats(Backend, &Stats);
  CHECK(Stats.bytes_in_use == 0);
#if MD_PERF_MINIMAL
  CHECK(Stats.acquire_calls == 0 && Stats.acquire_successes == 0);
#else
  CHECK(Stats.acquire_successes >= 2);
#endif
  md_backend_close(Backend);
  char Name[96]; std::snprintf(Name, sizeof(Name), "/%s_meta", Domain); shm_unlink(Name);
  std::snprintf(Name, sizeof(Name), "/%s_data", Domain); shm_unlink(Name);
  std::printf("CAPACITY_POLICY_PASS perf_minimal=%d\n", MD_PERF_MINIMAL);
}
