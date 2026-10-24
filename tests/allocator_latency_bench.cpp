#include "../include/latency_profile.h"
// Common latency workload for the Scudo and jemalloc delegation adapters.
// Build once with -DMD_BENCH_SCUDO and once with -DMD_BENCH_JEMALLOC.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>

#if defined(MD_BENCH_JEMALLOC)
#include <jemalloc/jemalloc.h>
#include "jemalloc_adapter.h"
extern "C" int md_jemalloc_owns(void *Address);
#elif !defined(MD_BENCH_SCUDO)
#error "define exactly one of MD_BENCH_SCUDO or MD_BENCH_JEMALLOC"
#else
#include "scudo_delegation.h"
#include <malloc.h>
extern "C" int __scudo_bench_is_primary(const void *);
extern "C" void __scudo_bench_cache_stats(uint64_t *, uint64_t *);
#ifndef M_CACHE_COUNT_MAX
#define M_CACHE_COUNT_MAX -200
#endif
#endif

extern "C" int md_scudo_checkpoint();
extern "C" const char *md_cpu_query_mode() __attribute__((weak));

#ifndef SCUDO_SECONDARY_STATS
#define SCUDO_SECONDARY_STATS 1
#endif

#ifndef MD_BENCH_BUILD_ID
#define MD_BENCH_BUILD_ID "unversioned"
#endif

namespace {

int allocatorCheckpoint(bool Delegated);

using Clock = std::chrono::steady_clock;

struct Options {
  size_t Size = 128 * 1024;
  uint64_t Iterations = 10;
  uint32_t Warmup = 5;
  uint32_t WorkersPerCore = 1;
  bool TouchPages = true;
  bool Delegated = false;
  bool ProcessMode = false;
  bool AllowFallback = false;
  bool CacheEnabled = true;
  std::vector<int> Cpus;
};

struct Samples {
  std::vector<uint64_t> Alloc;
  std::vector<uint64_t> Hold;
  std::vector<uint64_t> Dealloc;
  std::vector<uint64_t> Round;
  uint64_t Routes[8] = {}; // primary, delegated, secondary, cache calls, hits, fallback
  uint64_t Noise[4] = {}; // minor/major faults, voluntary/involuntary switches.
};

struct Stats {
  double Mean = 0;
  uint64_t P50 = 0;
  uint64_t P95 = 0;
  uint64_t P99 = 0;
};

class Barrier {
public:
  explicit Barrier(unsigned Count) : Remaining(Count) {}
  void wait(std::atomic<int> &Error, bool Delegated) {
    std::unique_lock<std::mutex> Lock(Mutex);
    if (--Remaining == 0) {
      int Result = allocatorCheckpoint(Delegated);
      if (Result) Error.store(Result);
      Ready = true;
      Cv.notify_all();
      return;
    }
    Cv.wait(Lock, [&] { return Ready; });
  }
private:
  std::mutex Mutex;
  std::condition_variable Cv;
  unsigned Remaining;
  bool Ready = false;
};

#if defined(MD_BENCH_JEMALLOC)
unsigned JemallocArena = 0;
#endif

int allocatorCheckpoint(bool Delegated) {
  if (!Delegated) return 0;
#if defined(MD_BENCH_JEMALLOC)
  return md_jemalloc_checkpoint();
#else
  return md_scudo_checkpoint();
#endif
}

void prepareSamples(Samples &Out, uint64_t Count) {
  // resize initializes every sample page before warmup and the start barrier.
  Out.Alloc.resize(Count);
  Out.Hold.resize(Count);
  Out.Dealloc.resize(Count);
  Out.Round.resize(Count);
}

uint64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch()).count();
}

void *allocateObject(size_t Size) {
  { MD_SCOPE(14, EmptyProbe); }
#if defined(MD_BENCH_JEMALLOC)
  return je_mallocx(Size, MALLOCX_ARENA(JemallocArena) | MALLOCX_TCACHE_NONE);
#else
  return std::malloc(Size);
#endif
}

void releaseObject(void *Pointer) {
#if defined(MD_BENCH_JEMALLOC)
  je_dallocx(Pointer, MALLOCX_TCACHE_NONE);
#else
  std::free(Pointer);
#endif
}

bool isDelegatedObject(void *Pointer) {
#if defined(MD_BENCH_JEMALLOC)
  return md_jemalloc_owns(Pointer) != 0;
#else
  return scudo::scudoDelegationContains(reinterpret_cast<uintptr_t>(Pointer));
#endif
}

bool pinToCpu(int Cpu) {
  if (Cpu < 0 || Cpu >= CPU_SETSIZE) { errno = EINVAL; return false; }
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(Cpu, &Set);
  return sched_setaffinity(0, sizeof(Set), &Set) == 0;
}

void touchPages(void *Pointer, size_t Size) {
  const long PageValue = sysconf(_SC_PAGESIZE);
  const size_t Page = PageValue > 0 ? static_cast<size_t>(PageValue) : 4096;
  auto *Bytes = static_cast<volatile unsigned char *>(Pointer);
  for (size_t Offset = 0; Offset < Size; Offset += Page)
    Bytes[Offset] = static_cast<unsigned char>(Offset / Page);
  if (Size)
    Bytes[Size - 1] ^= 1;
}

int runWarmup(const Options &Opt) {
  for (uint32_t I = 0; I < Opt.Warmup; ++I) {
    void *Pointer = allocateObject(Opt.Size);
    if (!Pointer)
      return ENOMEM;
    if (Opt.Delegated && !Opt.AllowFallback && !isDelegatedObject(Pointer)) {
      releaseObject(Pointer);
      return ENOTSUP;
    }
    if (Opt.TouchPages)
      touchPages(Pointer, Opt.Size);
    releaseObject(Pointer);
  }
  return 0;
}

void runWorker(const Options &Opt, int Cpu, Barrier *Start, Barrier *Finish,
               Samples &Out, std::atomic<int> &Error, bool Prepared = false) {
  int LocalError = 0;
  if (!Prepared) {
    if (!pinToCpu(Cpu)) LocalError = errno ? errno : EINVAL;
    prepareSamples(Out, Opt.Iterations);
    if (!LocalError) LocalError = runWarmup(Opt);
  }
  if (LocalError) Error.store(LocalError);
  if (Start) Start->wait(Error, Opt.Delegated);
  if (md_cpu_query_mode && !std::strncmp(md_cpu_query_mode(), "rseq_", 5))
    Out.Routes[6] = 1;
  else Out.Routes[7] = 1;
  rusage Before{}, After{};
  if (getrusage(RUSAGE_THREAD, &Before)) Error.store(errno);
  uint64_t CacheCallsBefore = 0, CacheHitsBefore = 0;
  const bool CountCache = Prepared || Opt.Cpus.size() * Opt.WorkersPerCore == 1;
#if defined(MD_BENCH_SCUDO)
  if (CountCache) __scudo_bench_cache_stats(&CacheCallsBefore, &CacheHitsBefore);
#endif
#if MD_PROFILE_STAGE
  uint64_t ProfileValues[4096] = {};
  mdprof::Buffer Profile{ProfileValues, 0, 4096};
  uint64_t Empty[500] = {};
  for (unsigned j=0;j<500;++j) { uint64_t a=mdprof::ticks(); Empty[j]=mdprof::ticks()-a; }
  std::sort(Empty,Empty+500);
#endif
  uint64_t Completed = 0;
  if (!Error.load()) {
    for (uint64_t I = 0; I < Opt.Iterations && !Error.load(); ++I) {
      const uint64_t T0 = nowNs();
#if MD_PROFILE_STAGE
      mdprof::Current = &Profile;
#endif
      void *Pointer = allocateObject(Opt.Size);
#if MD_PROFILE_STAGE
      mdprof::Current = nullptr;
#endif
      const uint64_t T1 = nowNs();
      if (!Pointer) { Error.store(ENOMEM); break; }
      if (Opt.Delegated && !Opt.AllowFallback && !isDelegatedObject(Pointer)) {
        releaseObject(Pointer);
        Error.store(ENOTSUP);
        break;
      }
#if defined(MD_BENCH_SCUDO)
      if (__scudo_bench_is_primary(Pointer)) ++Out.Routes[0];
      else if (isDelegatedObject(Pointer)) ++Out.Routes[1];
      else { ++Out.Routes[2]; if (Opt.Delegated) ++Out.Routes[5]; }
#endif
      if (Opt.TouchPages) touchPages(Pointer, Opt.Size);
      const uint64_t T2 = nowNs();
      releaseObject(Pointer);
      const uint64_t T3 = nowNs();
      Out.Alloc[I] = T1 - T0;
      Out.Hold[I] = T2 - T1;
      Out.Dealloc[I] = T3 - T2;
      Out.Round[I] = T3 - T0;
      ++Completed;
    }
  }
#if defined(MD_BENCH_SCUDO)
  uint64_t CacheCallsAfter = 0, CacheHitsAfter = 0;
  if (CountCache) __scudo_bench_cache_stats(&CacheCallsAfter, &CacheHitsAfter);
  // The allocator counters are u32. Unsigned subtraction handles wraparound.
  Out.Routes[3] = static_cast<uint32_t>(CacheCallsAfter - CacheCallsBefore);
  Out.Routes[4] = static_cast<uint32_t>(CacheHitsAfter - CacheHitsBefore);
#else
  (void)CountCache; (void)CacheCallsBefore; (void)CacheHitsBefore;
#endif
  if (getrusage(RUSAGE_THREAD, &After)) Error.store(errno);
  Out.Noise[0] = After.ru_minflt - Before.ru_minflt;
  Out.Noise[1] = After.ru_majflt - Before.ru_majflt;
  Out.Noise[2] = After.ru_nvcsw - Before.ru_nvcsw;
  Out.Noise[3] = After.ru_nivcsw - Before.ru_nivcsw;
#if MD_PROFILE_STAGE
  long double sum=0; for(uint64_t j=0;j<Profile.Count;++j) sum+=ProfileValues[j];
  std::sort(ProfileValues,ProfileValues+Profile.Count);
  auto q=[&](double x) {return Profile.Count? ProfileValues[uint64_t(x*(Profile.Count-1))]:0;};
  std::printf("PROFILE stage=%d pid=%d count=%llu frequency=%llu empty_p50=%llu mean_ticks=%.4Lf p50_ticks=%llu p95_ticks=%llu p99_ticks=%llu max_ticks=%llu\n", MD_PROFILE_STAGE, getpid(), (unsigned long long)Profile.Count, (unsigned long long)mdprof::frequency(), (unsigned long long)Empty[250], Profile.Count?sum/Profile.Count:0, (unsigned long long)q(.5), (unsigned long long)q(.95), (unsigned long long)q(.99), (unsigned long long)(Profile.Count?ProfileValues[Profile.Count-1]:0));
#endif
  // All measured workers finish before any thread performs the final drain.
  if (Finish) Finish->wait(Error, Opt.Delegated);
  Out.Alloc.resize(Completed);
  Out.Hold.resize(Completed);
  Out.Dealloc.resize(Completed);
  Out.Round.resize(Completed);
}

bool writeExact(int Fd, const void *Data, size_t Size) {
  const auto *Bytes = static_cast<const unsigned char *>(Data);
  while (Size) {
    ssize_t Written = write(Fd, Bytes, Size);
    if (Written < 0 && errno == EINTR)
      continue;
    if (Written <= 0)
      return false;
    Bytes += Written;
    Size -= static_cast<size_t>(Written);
  }
  return true;
}

bool readExact(int Fd, void *Data, size_t Size) {
  auto *Bytes = static_cast<unsigned char *>(Data);
  while (Size) {
    ssize_t Got = read(Fd, Bytes, Size);
    if (Got < 0 && errno == EINTR)
      continue;
    if (Got <= 0)
      return false;
    Bytes += Got;
    Size -= static_cast<size_t>(Got);
  }
  return true;
}

bool sendSamples(int Fd, const Samples &Value, int Error) {
  if (!writeExact(Fd, Value.Routes, sizeof(Value.Routes))) return false;
  if (!writeExact(Fd, Value.Noise, sizeof(Value.Noise))) return false;
  if (!writeExact(Fd, &Error, sizeof(Error)))
    return false;
  const std::vector<uint64_t> *Vectors[] = {
      &Value.Alloc, &Value.Hold, &Value.Dealloc, &Value.Round};
  for (const auto *Vector : Vectors) {
    uint64_t Count = Vector->size();
    if (!writeExact(Fd, &Count, sizeof(Count)) ||
        (Count && !writeExact(Fd, Vector->data(), Count * sizeof(uint64_t))))
      return false;
  }
  return true;
}

bool receiveSamples(int Fd, Samples &Value, int &Error) {
  if (!readExact(Fd, Value.Routes, sizeof(Value.Routes))) return false;
  if (!readExact(Fd, Value.Noise, sizeof(Value.Noise))) return false;
  if (!readExact(Fd, &Error, sizeof(Error)))
    return false;
  std::vector<uint64_t> *Vectors[] = {
      &Value.Alloc, &Value.Hold, &Value.Dealloc, &Value.Round};
  for (auto *Vector : Vectors) {
    uint64_t Count = 0;
    if (!readExact(Fd, &Count, sizeof(Count)))
      return false;
    Vector->resize(Count);
    if (Count && !readExact(Fd, Vector->data(), Count * sizeof(uint64_t)))
      return false;
  }
  return true;
}

void appendSamples(Samples &To, Samples &&From) {
  for (unsigned I = 0; I < 8; ++I) To.Routes[I] += From.Routes[I];
  for (unsigned I = 0; I < 4; ++I) To.Noise[I] += From.Noise[I];
  auto Append = [](std::vector<uint64_t> &Dst, std::vector<uint64_t> &Src) {
    Dst.insert(Dst.end(), std::make_move_iterator(Src.begin()),
               std::make_move_iterator(Src.end()));
  };
  Append(To.Alloc, From.Alloc);
  Append(To.Hold, From.Hold);
  Append(To.Dealloc, From.Dealloc);
  Append(To.Round, From.Round);
}

int runThreads(const Options &Opt, Samples &Combined) {
  const unsigned Workers = Opt.Cpus.size() * Opt.WorkersPerCore;
  Barrier Start(Workers), Finish(Workers);
  std::atomic<int> Error{0};
  std::vector<Samples> PerWorker(Workers);
  std::vector<std::thread> Threads;
  Threads.reserve(Workers);
  for (unsigned I = 0; I < Workers; ++I) {
    const int Cpu = Opt.Cpus[I % Opt.Cpus.size()];
    Threads.emplace_back(runWorker, std::cref(Opt), Cpu, &Start, &Finish,
                         std::ref(PerWorker[I]), std::ref(Error), false);
  }
  for (auto &Thread : Threads)
    Thread.join();
  if (Error.load())
    return Error.load();
  for (auto &Value : PerWorker)
    appendSamples(Combined, std::move(Value));
  return 0;
}

int runProcesses(const Options &Opt, const char *SelfExe, Samples &Combined) {
  const unsigned Workers = Opt.Cpus.size() * Opt.WorkersPerCore;
  struct Child { pid_t Pid; int ReadFd; int StartFd; };
  std::vector<Child> Children;
  struct Cleanup {
    std::vector<Child> &List;
    ~Cleanup() {
      for (auto &ChildValue : List) {
        if (ChildValue.StartFd >= 0) close(ChildValue.StartFd);
        if (ChildValue.ReadFd >= 0) close(ChildValue.ReadFd);
        if (ChildValue.Pid > 0) kill(ChildValue.Pid, SIGTERM);
      }
      for (auto &ChildValue : List)
        if (ChildValue.Pid > 0)
          while (waitpid(ChildValue.Pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
  } Guard{Children};
  Children.reserve(Workers);
  for (unsigned I = 0; I < Workers; ++I) {
    int Fds[2];
    int StartFds[2];
    if (pipe(Fds) || pipe(StartFds))
      return errno;
    const pid_t Pid = fork();
    if (Pid < 0)
      return errno;
    if (Pid == 0) {
      close(Fds[0]);
      close(StartFds[1]);
      char OutFd[32], StartFd[32], Cpu[32], Size[32], Iterations[32], Warmup[32];
      std::snprintf(OutFd, sizeof(OutFd), "%d", Fds[1]);
      std::snprintf(StartFd, sizeof(StartFd), "%d", StartFds[0]);
      std::snprintf(Cpu, sizeof(Cpu), "%d", Opt.Cpus[I % Opt.Cpus.size()]);
      std::snprintf(Size, sizeof(Size), "%zu", Opt.Size);
      std::snprintf(Iterations, sizeof(Iterations), "%" PRIu64, Opt.Iterations);
      std::snprintf(Warmup, sizeof(Warmup), "%u", Opt.Warmup);
      execl(SelfExe, SelfExe, "--role", "worker", "--out-fd", OutFd,
            "--start-fd", StartFd, "--cpu", Cpu, "--size", Size,
            "--iterations", Iterations, "--warmup", Warmup,
            "--touch-pages", Opt.TouchPages ? "1" : "0",
            "--path", Opt.Delegated ? "delegated" : "traditional",
            "--allow-fallback", Opt.AllowFallback ? "1" : "0",
            "--cache", Opt.CacheEnabled ? "enabled" : "disabled",
            static_cast<char *>(nullptr));
      _exit(127);
    }
    close(Fds[1]);
    close(StartFds[0]);
    Children.push_back({Pid, Fds[0], StartFds[1]});
  }
  for (auto &ChildValue : Children) {
    unsigned char ReadyByte = 0;
    if (!readExact(ChildValue.ReadFd, &ReadyByte, sizeof(ReadyByte)) || ReadyByte != 1)
      return EIO;
  }
  int FirstError = 0;
  const unsigned char DrainByte = 'D';
  for (auto &ChildValue : Children)
    if (!writeExact(ChildValue.StartFd, &DrainByte, 1)) return EIO;
  for (auto &ChildValue : Children) {
    int ChildError = 0;
    if (!readExact(ChildValue.ReadFd, &ChildError, sizeof(ChildError))) return EIO;
    if (ChildError && !FirstError) FirstError = ChildError;
  }
  const unsigned char StartByte = 'S';
  for (auto &ChildValue : Children) {
    if (!writeExact(ChildValue.StartFd, &StartByte, sizeof(StartByte)))
      return EIO;
  }
  for (auto &ChildValue : Children) {
    unsigned char DoneByte = 0;
    if (!readExact(ChildValue.ReadFd, &DoneByte, 1) || DoneByte != 'E') return EIO;
  }
  const unsigned char FinishByte = 'F';
  for (auto &ChildValue : Children) {
    if (!writeExact(ChildValue.StartFd, &FinishByte, 1)) return EIO;
    close(ChildValue.StartFd);
    ChildValue.StartFd = -1;
  }
  for (auto &ChildValue : Children) {
    Samples Local;
    int ChildError = 0;
    if (!receiveSamples(ChildValue.ReadFd, Local, ChildError) && !FirstError)
      FirstError = EIO;
    close(ChildValue.ReadFd);
    ChildValue.ReadFd = -1;
    int Status = 0;
    while (waitpid(ChildValue.Pid, &Status, 0) < 0 && errno == EINTR) {}
    ChildValue.Pid = -1;
    if ((!WIFEXITED(Status) || WEXITSTATUS(Status) != 0) && !FirstError)
      FirstError = ChildError ? ChildError : ECHILD;
    if (ChildError && !FirstError)
      FirstError = ChildError;
    appendSamples(Combined, std::move(Local));
  }
  return FirstError;
}

Stats summarize(std::vector<uint64_t> Values) {
  Stats Result;
  if (Values.empty())
    return Result;
  long double Sum = 0;
  for (uint64_t Value : Values)
    Sum += Value;
  Result.Mean = static_cast<double>(Sum / Values.size());
  std::sort(Values.begin(), Values.end());
  auto Percentile = [&](double P) {
    size_t Index = static_cast<size_t>(P * static_cast<double>(Values.size() - 1));
    return Values[Index];
  };
  Result.P50 = Percentile(0.50);
  Result.P95 = Percentile(0.95);
  Result.P99 = Percentile(0.99);
  return Result;
}

void printMetric(const char *Name, const std::vector<uint64_t> &Values) {
  const Stats Result = summarize(Values);
  std::printf("\t%-12s\tmean=%.2f p50=%" PRIu64 " p95=%" PRIu64
              " p99=%" PRIu64 "\n", Name, Result.Mean, Result.P50,
              Result.P95, Result.P99);
}

std::vector<int> parseCpus(const char *Text) {
  std::vector<int> Result;
  const char *Cursor = Text;
  while (*Cursor) {
    char *End = nullptr;
    long Cpu = std::strtol(Cursor, &End, 10);
    if (End == Cursor || Cpu < 0 || Cpu >= CPU_SETSIZE)
      return {};
    Result.push_back(static_cast<int>(Cpu));
    if (!*End)
      break;
    if (*End != ',')
      return {};
    Cursor = End + 1;
  }
  return Result;
}

const char *humanUnit(size_t Size, double &Value) {
  if (Size >= (1U << 20)) {
    Value = static_cast<double>(Size) / (1U << 20);
    return "MiB";
  }
  if (Size >= 1024) {
    Value = static_cast<double>(Size) / 1024;
    return "KiB";
  }
  Value = static_cast<double>(Size);
  return "B";
}

void usage(const char *Argv0) {
  std::printf(
      "Usage: %s --path traditional|delegated --mode thread|process [options]\n"
      "  --allow-fallback 0|1    allow Primary/ordinary Secondary, report actual routes\n"
      "  --cache enabled|disabled  control ordinary Secondary cache\n"
      "  --size BYTES             one allocation size\n"
      "  --iterations N           measured rounds per worker\n"
      "  --warmup N               warmup rounds per worker\n"
      "  --touch-pages 0|1        touch one byte per page\n"
      "  --cpus C0,C1,...         physical-core representatives\n"
      "  --workers-per-core N     threads or processes pinned per core\n",
      Argv0);
}

} // namespace

int main(int Argc, char **Argv) {
  if (Argc == 2 && !std::strcmp(Argv[1], "--build-info")) {
    std::printf("bench_protocol=shared_rings_v3 route_protocol=scudo_routes_v1 build_id=%s scudo_secondary_stats=%d\n", MD_BENCH_BUILD_ID, SCUDO_SECONDARY_STATS);
    return 0;
  }
  Options Opt;
  const char *Role = nullptr;
  int RoleOutFd = -1;
  int RoleStartFd = -1;
  int RoleCpu = -1;
  for (int I = 1; I < Argc; ++I) {
    auto Need = [&](const char *Name) -> const char * {
      if (I + 1 >= Argc) {
        std::fprintf(stderr, "missing value for %s\n", Name);
        std::exit(2);
      }
      return Argv[++I];
    };
    if (!std::strcmp(Argv[I], "--size"))
      Opt.Size = std::strtoull(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--iterations"))
      Opt.Iterations = std::strtoull(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--warmup"))
      Opt.Warmup = std::strtoul(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--allow-fallback"))
      Opt.AllowFallback = !std::strcmp(Need(Argv[I]), "1");
    else if (!std::strcmp(Argv[I], "--cache")) {
      const char *Value = Need(Argv[I]);
      if (std::strcmp(Value, "enabled") && std::strcmp(Value, "disabled")) return 2;
      Opt.CacheEnabled = !std::strcmp(Value, "enabled");
    }
    else if (!std::strcmp(Argv[I], "--touch-pages"))
      Opt.TouchPages = !std::strcmp(Need(Argv[I]), "1");
    else if (!std::strcmp(Argv[I], "--workers-per-core"))
      Opt.WorkersPerCore = std::strtoul(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--cpus"))
      Opt.Cpus = parseCpus(Need(Argv[I]));
    else if (!std::strcmp(Argv[I], "--path"))
      Opt.Delegated = !std::strcmp(Need(Argv[I]), "delegated");
    else if (!std::strcmp(Argv[I], "--mode"))
      Opt.ProcessMode = !std::strcmp(Need(Argv[I]), "process");
    else if (!std::strcmp(Argv[I], "--role"))
      Role = Need(Argv[I]);
    else if (!std::strcmp(Argv[I], "--out-fd"))
      RoleOutFd = std::strtol(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--start-fd"))
      RoleStartFd = std::strtol(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--cpu"))
      RoleCpu = std::strtol(Need(Argv[I]), nullptr, 0);
    else if (!std::strcmp(Argv[I], "--help") || !std::strcmp(Argv[I], "-h")) {
      usage(Argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", Argv[I]);
      return 2;
    }
  }
  if (!Opt.Size || !Opt.Iterations || !Opt.WorkersPerCore ||
      (!Role && Opt.Cpus.empty())) {
    usage(Argv[0]);
    return 2;
  }

#if defined(MD_BENCH_SCUDO)
  if (!Opt.CacheEnabled && !mallopt(M_CACHE_COUNT_MAX, 0)) {
    std::fprintf(stderr, "cannot disable Secondary cache\n");
    return 1;
  }
#endif
#if defined(MD_BENCH_JEMALLOC)
  if (Opt.Delegated && (!Opt.ProcessMode || Role != nullptr)) {
    const char *Domain = std::getenv("MEMORY_DELEGATION_DOMAIN");
    if (!Domain || md_jemalloc_init(Domain, &JemallocArena)) {
      std::fprintf(stderr, "md_jemalloc_init failed\n");
      return 1;
    }
  }
#endif

  if (Role) {
    if (std::strcmp(Role, "worker") || RoleOutFd < 0 || RoleStartFd < 0 ||
        RoleCpu < 0)
      return 2;
    std::atomic<int> Error{0};
    Samples Local;
    if (!pinToCpu(RoleCpu)) Error.store(errno ? errno : EINVAL);
    prepareSamples(Local, Opt.Iterations);
    if (!Error.load()) Error.store(runWarmup(Opt));
    const unsigned char ReadyByte = 1;
    if (!writeExact(RoleOutFd, &ReadyByte, 1)) return 1;
    unsigned char Command = 0;
    if (!readExact(RoleStartFd, &Command, 1) || Command != 'D') return 1;
    int CheckError = allocatorCheckpoint(Opt.Delegated);
    if (CheckError) Error.store(CheckError);
    CheckError = Error.load();
    if (!writeExact(RoleOutFd, &CheckError, sizeof(CheckError))) return 1;
    if (!readExact(RoleStartFd, &Command, 1) || Command != 'S') return 1;
    runWorker(Opt, RoleCpu, nullptr, nullptr, Local, Error, true);
    const unsigned char DoneByte = 'E';
    if (!writeExact(RoleOutFd, &DoneByte, 1)) return 1;
    if (!readExact(RoleStartFd, &Command, 1) || Command != 'F') return 1;
    close(RoleStartFd);
    CheckError = allocatorCheckpoint(Opt.Delegated);
    if (CheckError) Error.store(CheckError);
    const bool Ok = sendSamples(RoleOutFd, Local, Error.load());
    close(RoleOutFd);
    return Ok && !Error.load() ? 0 : 1;
  }

  signal(SIGPIPE, SIG_IGN);
  Samples Combined;
  char SelfExe[PATH_MAX];
  const ssize_t SelfLength = readlink("/proc/self/exe", SelfExe,
                                      sizeof(SelfExe) - 1);
  if (SelfLength <= 0 || static_cast<size_t>(SelfLength) >= sizeof(SelfExe) - 1) {
    std::fprintf(stderr, "cannot resolve /proc/self/exe\n");
    return 1;
  }
  SelfExe[SelfLength] = '\0';
  const int Error = Opt.ProcessMode ? runProcesses(Opt, SelfExe, Combined)
                                    : runThreads(Opt, Combined);
  if (Error) {
    std::fprintf(stderr, "benchmark failed: %s (%d)\n", std::strerror(Error), Error);
    return 1;
  }
  double Human = 0;
  const char *Unit = humanUnit(Opt.Size, Human);
  const unsigned Workers = Opt.Cpus.size() * Opt.WorkersPerCore;
  std::printf("================== size=%.2f %s, %s=%u, unit=ns ==================\n",
              Human, Unit, Opt.ProcessMode ? "processes" : "threads", Workers);
  std::printf("bench_config: mode=%s cores=%zu workers_per_core=%u touch_pages=%d\n",
              Opt.ProcessMode ? "process" : "thread", Opt.Cpus.size(),
              Opt.WorkersPerCore, Opt.TouchPages ? 1 : 0);
  std::printf("bench_lifecycle: protocol=shared_rings_v3 warmup=%u iterations=%" PRIu64
              " pre_checkpoint=ok post_checkpoint=ok build_id=%s scudo_secondary_stats=%d\n",
              Opt.Warmup, Opt.Iterations, MD_BENCH_BUILD_ID, SCUDO_SECONDARY_STATS);
  std::printf("-- %s --\n", Opt.Delegated ? "delegated" : "traditional");
  std::printf("bench_noise: worker_sum minor_faults=%" PRIu64 " major_faults=%" PRIu64
              " voluntary_cs=%" PRIu64 " involuntary_cs=%" PRIu64 "\n",
              Combined.Noise[0], Combined.Noise[1], Combined.Noise[2], Combined.Noise[3]);
#if defined(MD_BENCH_SCUDO)
  std::printf("bench_routes: primary=%" PRIu64 " delegated=%" PRIu64
              " secondary=%" PRIu64 " cache_calls=%" PRIu64 " cache_hits=%" PRIu64
              " fallback=%" PRIu64 " samples=%zu cache=%s allow_fallback=%d"
              " rseq_workers=%" PRIu64 " fallback_cpu_workers=%" PRIu64 "\n",
              Combined.Routes[0], Combined.Routes[1], Combined.Routes[2],
              Combined.Routes[3], Combined.Routes[4], Combined.Routes[5],
              Combined.Alloc.size(), Opt.CacheEnabled ? "enabled" : "disabled",
              Opt.AllowFallback ? 1 : 0, Combined.Routes[6], Combined.Routes[7]);
#endif
  printMetric("alloc_only", Combined.Alloc);
  printMetric("hold_only", Combined.Hold);
  printMetric("dealloc_only", Combined.Dealloc);
  printMetric("all", Combined.Round);
  return 0;
}
