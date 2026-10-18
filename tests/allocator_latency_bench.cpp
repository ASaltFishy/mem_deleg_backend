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
#include <sys/wait.h>
#include <unistd.h>

#if defined(MD_BENCH_JEMALLOC)
#include <jemalloc/jemalloc.h>
#include "jemalloc_adapter.h"
#elif !defined(MD_BENCH_SCUDO)
#error "define exactly one of MD_BENCH_SCUDO or MD_BENCH_JEMALLOC"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  size_t Size = 128 * 1024;
  uint64_t Iterations = 200;
  uint32_t Warmup = 50;
  uint32_t WorkersPerCore = 1;
  bool TouchPages = true;
  bool Delegated = false;
  bool ProcessMode = false;
  std::vector<int> Cpus;
};

struct Samples {
  std::vector<uint64_t> Alloc;
  std::vector<uint64_t> Hold;
  std::vector<uint64_t> Dealloc;
  std::vector<uint64_t> Round;
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
  void wait() {
    std::unique_lock<std::mutex> Lock(Mutex);
    if (--Remaining == 0) {
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

uint64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch()).count();
}

void *allocateObject(size_t Size) {
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

bool pinToCpu(int Cpu) {
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
    if (Opt.TouchPages)
      touchPages(Pointer, Opt.Size);
    releaseObject(Pointer);
  }
  return 0;
}

void runWorker(const Options &Opt, int Cpu, Barrier *Start, Samples &Out,
               std::atomic<int> &Error) {
  int LocalError = 0;
  if (!pinToCpu(Cpu))
    LocalError = errno ? errno : EINVAL;
  if (!LocalError)
    LocalError = runWarmup(Opt);
  if (LocalError)
    Error.store(LocalError);
  if (Start)
    Start->wait();
  if (LocalError)
    return;
  Out.Alloc.reserve(Opt.Iterations);
  Out.Hold.reserve(Opt.Iterations);
  Out.Dealloc.reserve(Opt.Iterations);
  Out.Round.reserve(Opt.Iterations);
  for (uint64_t I = 0; I < Opt.Iterations; ++I) {
    const uint64_t T0 = nowNs();
    void *Pointer = allocateObject(Opt.Size);
    const uint64_t T1 = nowNs();
    if (!Pointer) {
      Error.store(ENOMEM);
      return;
    }
    if (Opt.TouchPages)
      touchPages(Pointer, Opt.Size);
    const uint64_t T2 = nowNs();
    releaseObject(Pointer);
    const uint64_t T3 = nowNs();
    Out.Alloc.push_back(T1 - T0);
    Out.Hold.push_back(T2 - T1);
    Out.Dealloc.push_back(T3 - T2);
    Out.Round.push_back(T3 - T0);
  }
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
  Barrier Start(Workers);
  std::atomic<int> Error{0};
  std::vector<Samples> PerWorker(Workers);
  std::vector<std::thread> Threads;
  Threads.reserve(Workers);
  for (unsigned I = 0; I < Workers; ++I) {
    const int Cpu = Opt.Cpus[I % Opt.Cpus.size()];
    Threads.emplace_back(runWorker, std::cref(Opt), Cpu, &Start,
                         std::ref(PerWorker[I]), std::ref(Error));
  }
  for (auto &Thread : Threads)
    Thread.join();
  if (Error.load())
    return Error.load();
  for (auto &Value : PerWorker)
    appendSamples(Combined, std::move(Value));
  return 0;
}

int runProcesses(const Options &Opt, Samples &Combined) {
  const unsigned Workers = Opt.Cpus.size() * Opt.WorkersPerCore;
  struct Child { pid_t Pid; int ReadFd; int StartFd; };
  std::vector<Child> Children;
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
      Options ChildOpt = Opt;
      std::atomic<int> Error{0};
      if (!pinToCpu(Opt.Cpus[I % Opt.Cpus.size()]))
        Error.store(errno ? errno : EINVAL);
      if (!Error.load())
        Error.store(runWarmup(Opt));
      const unsigned char ReadyByte = 1;
      if (!writeExact(Fds[1], &ReadyByte, sizeof(ReadyByte)))
        _exit(1);
      unsigned char StartByte = 0;
      if (!readExact(StartFds[0], &StartByte, sizeof(StartByte)))
        _exit(1);
      close(StartFds[0]);
      Samples Local;
      ChildOpt.Warmup = 0;
      if (!Error.load())
        runWorker(ChildOpt, Opt.Cpus[I % Opt.Cpus.size()], nullptr, Local, Error);
      const bool Ok = sendSamples(Fds[1], Local, Error.load());
      close(Fds[1]);
      _exit(Ok && !Error.load() ? 0 : 1);
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
  const unsigned char StartByte = 1;
  for (auto &ChildValue : Children) {
    if (!writeExact(ChildValue.StartFd, &StartByte, sizeof(StartByte)))
      return EIO;
    close(ChildValue.StartFd);
  }
  int FirstError = 0;
  for (auto &ChildValue : Children) {
    Samples Local;
    int ChildError = 0;
    if (!receiveSamples(ChildValue.ReadFd, Local, ChildError) && !FirstError)
      FirstError = EIO;
    close(ChildValue.ReadFd);
    int Status = 0;
    while (waitpid(ChildValue.Pid, &Status, 0) < 0 && errno == EINTR) {}
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
    if (End == Cursor || Cpu < 0)
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
  Options Opt;
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
    else if (!std::strcmp(Argv[I], "--help") || !std::strcmp(Argv[I], "-h")) {
      usage(Argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", Argv[I]);
      return 2;
    }
  }
  if (!Opt.Size || !Opt.Iterations || !Opt.WorkersPerCore || Opt.Cpus.empty()) {
    usage(Argv[0]);
    return 2;
  }

#if defined(MD_BENCH_JEMALLOC)
  if (Opt.Delegated) {
    const char *Domain = std::getenv("MEMORY_DELEGATION_DOMAIN");
    if (!Domain || md_jemalloc_init(Domain, &JemallocArena)) {
      std::fprintf(stderr, "md_jemalloc_init failed\n");
      return 1;
    }
  }
#endif

  Samples Combined;
  const int Error = Opt.ProcessMode ? runProcesses(Opt, Combined)
                                    : runThreads(Opt, Combined);
  if (Error) {
    std::fprintf(stderr, "benchmark failed: %s (%d)\n", std::strerror(Error), Error);
    return 1;
  }
  double Human = 0;
  const char *Unit = humanUnit(Opt.Size, Human);
  const unsigned Workers = Opt.Cpus.size() * Opt.WorkersPerCore;
  std::printf("================== size=%.2f %s, threads=%u, unit=ns ==================\n",
              Human, Unit, Workers);
  std::printf("bench_config: mode=%s cores=%zu workers_per_core=%u touch_pages=%d\n",
              Opt.ProcessMode ? "process" : "thread", Opt.Cpus.size(),
              Opt.WorkersPerCore, Opt.TouchPages ? 1 : 0);
  std::printf("-- %s --\n", Opt.Delegated ? "delegated" : "traditional");
  printMetric("alloc_only", Combined.Alloc);
  printMetric("hold_only", Combined.Hold);
  printMetric("dealloc_only", Combined.Dealloc);
  printMetric("all", Combined.Round);
  return 0;
}
