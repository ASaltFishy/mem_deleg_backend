#include "delegation_backend.h"
#include "kernel_bridge.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>

#define CHECK(X) do { if (!(X)) { fprintf(stderr,"FAIL(backend-v2): line=%d %s errno=%d\n",__LINE__,#X,errno); exit(1); } } while (0)
static unsigned Home, Remote;
static std::atomic<int> Migrate{0};
static void pin(unsigned Cpu) { cpu_set_t Set; CPU_ZERO(&Set); CPU_SET(Cpu,&Set); CHECK(!sched_setaffinity(0,sizeof(Set),&Set)); }
extern "C" void md_test_metadata_locked() { if (Migrate.exchange(0)) pin(Remote); }
struct Job { md_backend *B; md_lease L; unsigned Cpu; int Result; int Clear; };
static void *releaseRemote(void *P) { auto &J=*static_cast<Job*>(P); pin(J.Cpu); J.Result=md_release(J.B,&J.L,J.Clear,nullptr); return nullptr; }
static void *childAcquire(void *P) {
  auto &J=*static_cast<Job*>(P); pin(J.Cpu);
  md_lease L{}; J.Result=md_acquire(J.B,65536,4096,md_process_id(),&L);
  if(!J.Result) { memset((void*)L.address,0x29,L.size); J.Result=md_release(J.B,&L,0,nullptr); }
  return nullptr;
}
static void checkpoint(md_backend *B) { CHECK(md_backend_checkpoint(B)==0); }
int main() {
  setbuf(stdout,nullptr);
  cpu_set_t Allowed; CHECK(!sched_getaffinity(0,sizeof(Allowed),&Allowed));
  Home=Remote=UINT32_MAX;
  for(unsigned C=0;C<CPU_SETSIZE;++C) if(CPU_ISSET(C,&Allowed)) { if(Home==UINT32_MAX) Home=C; else if(Remote==UINT32_MAX) Remote=C; }
  CHECK(Remote!=UINT32_MAX); pin(Home);
  const char *Domain=getenv("MEMORY_DELEGATION_DOMAIN"); CHECK(Domain);
  md_config Config{Domain,0x2000000000ULL,MD_DEFAULT_ARENA_CAPACITY,0};
  md_backend *B=nullptr; int Error=md_backend_open(&B,&Config); CHECK(Error==0 && B);
  md_stats Stats{}; md_get_stats(B,&Stats);
  bool Kernel=Stats.kernel_enabled!=0;
  CHECK(!getenv("EXPECT_KERNEL") || Kernel);
  printf("BACKEND_V2 kernel_arenas=%llu\n",(unsigned long long)Stats.kernel_enabled);
  uint32_t Owner=md_process_id();
  md_lease Whole{},Small{};
  CHECK(!md_acquire(B,Config.capacity,4096,Owner,&Whole));
  memset((void*)Whole.address,0x5a,4096);
  pin(Remote); CHECK(!md_release(B,&Whole,0,nullptr));
  md_get_stats(B,&Stats); CHECK(Stats.bytes_in_use==Config.capacity); // no owner-user reclaim yet
  pin(Home); checkpoint(B);
  CHECK(!md_acquire(B,65536,65536,Owner,&Small)); CHECK(Small.address==Whole.address);
  CHECK(*(unsigned char*)Small.address==0x5a);
  CHECK(Small.generation!=Whole.generation || Small.allocation_id!=Whole.allocation_id);
  CHECK(!md_release(B,&Small,0,nullptr)); checkpoint(B);
  puts("PASS(backend-v2): remote FREE retained until owner-user reclaim; split and reuse");
  // A local FREE must return a reusable candidate without waiting for receipt.
  for (unsigned I=0; I<64; ++I) {
    md_candidate Candidate{};
    CHECK(!md_acquire(B,65536,4096,Owner,&Small));
    uintptr_t Address=Small.address;
    CHECK(!md_release(B,&Small,0,&Candidate));
    CHECK(Candidate.block_id != UINT32_MAX);
    CHECK(!md_try_acquire(B,Candidate,Owner,&Small));
    CHECK(Small.address==Address);
    CHECK(!md_release(B,&Small,0,nullptr));
  }
  checkpoint(B); md_get_stats(B,&Stats); CHECK(!Stats.bytes_in_use);
  puts("PASS(backend-v2): same-core FREE immediately reusable; delayed receipts never double-publish");

  md_lease Current{}; CHECK(!md_acquire(B,Config.capacity,4096,Owner,&Current));
  memset((void*)Current.address,0x42,4096);
  pthread_t Threads[8]; Job Jobs[8];
  for(unsigned I=0;I<8;++I) { Jobs[I]={B,Whole,Remote,-1,1}; CHECK(!pthread_create(&Threads[I],nullptr,releaseRemote,&Jobs[I])); }
  for(unsigned I=0;I<8;++I) { CHECK(!pthread_join(Threads[I],nullptr)); CHECK(Jobs[I].Result==0); }
  checkpoint(B); md_get_stats(B,&Stats); CHECK(Stats.kernel_rejected>=8);
  CHECK(*(unsigned char*)Current.address==0x42); CHECK(!md_release(B,&Current,0,nullptr)); checkpoint(B);
  puts("PASS(backend-v2): stale remote FREE/clear cannot change newer lease payload");
  CHECK(!md_acquire(B,8192,4096,Owner,&Small)); memset((void*)Small.address,0x73,8192);
  pin(Remote); CHECK(!md_release(B,&Small,1,nullptr)); pin(Home); checkpoint(B);
  CHECK(!md_acquire(B,8192,4096,Owner,&Small));
  for(unsigned I=0;I<8192;++I) CHECK(((unsigned char*)Small.address)[I]==0);
  CHECK(!md_release(B,&Small,0,nullptr)); checkpoint(B);
  puts("PASS(backend-v2): requested remote clear executes only after validated receipt");

#ifdef MD_TEST_MIGRATION
  Migrate.store(1); CHECK(!md_acquire(B,65536,4096,Owner,&Small));
  CHECK(sched_getcpu()==(int)Remote); CHECK(Small.block_id/8192==Home);
  CHECK(!md_release(B,&Small,0,nullptr)); pin(Home); checkpoint(B);
  CHECK(!md_acquire(B,65536,4096,Owner,&Small));
  md_candidate MigratedCandidate{};
  Migrate.store(1); CHECK(!md_release(B,&Small,0,&MigratedCandidate));
  CHECK(sched_getcpu()==(int)Remote); CHECK(MigratedCandidate.block_id==UINT32_MAX);
  md_get_stats(B,&Stats); CHECK(Stats.bytes_in_use==65536);
  pin(Home); checkpoint(B);
  puts("PASS(backend-v2): forced migration while Mutex held; FREE acknowledged before reuse");
#endif

  CHECK(!md_acquire(B,Config.capacity,4096,Owner,&Whole));
  memset((void*)Whole.address,0x61,4096); CHECK(!md_release(B,&Whole,0,nullptr)); checkpoint(B);
  pid_t Child=fork(); CHECK(Child>=0);
  if(!Child) {
    md_lease L{}; CHECK(!md_acquire(B,65536,4096,md_process_id(),&L));
    CHECK(L.address==Whole.address);
    for(unsigned I=0;I<4096;++I) CHECK(((unsigned char*)L.address)[I]==0x61);
    memset((void*)L.address,0x37,4096); CHECK(!md_release(B,&L,0,nullptr)); checkpoint(B); _exit(0);
  }
  int Status; CHECK(waitpid(Child,&Status,0)==Child && WIFEXITED(Status) && !WEXITSTATUS(Status));
  CHECK(!md_acquire(B,65536,4096,Owner,&Small));
  for(unsigned I=0;I<4096;++I) CHECK(((unsigned char*)Small.address)[I]==0x37);
  if(Kernel) {
    Child=fork(); CHECK(Child>=0);
    if(!Child) { checkpoint(B); usleep(10000); (void)*(volatile unsigned char*)Small.address; _exit(90); }
    CHECK(waitpid(Child,&Status,0)==Child && WIFSIGNALED(Status) && WTERMSIG(Status)==SIGSEGV);
  }
  CHECK(!md_release(B,&Small,0,nullptr)); checkpoint(B);
  CHECK(!md_acquire_flags(B,65536,4096,Owner,MD_ZERO,&Small));
  for(unsigned I=0;I<4096;++I) CHECK(((unsigned char*)Small.address)[I]==0);
  CHECK(!md_release(B,&Small,0,nullptr)); checkpoint(B);
  puts("PASS(backend-v2): explicit MD_ZERO semantics preserved");
  puts("PASS(backend-v2): live processes reuse same VA, handoff clearing disabled, fork identity isolated");
  Child=fork(); CHECK(Child>=0);
  if(!Child) {
    for(unsigned I=0;I<4;++I) { Jobs[I]={B,{},Home,-1,0}; CHECK(!pthread_create(&Threads[I],nullptr,childAcquire,&Jobs[I])); }
    for(unsigned I=0;I<4;++I) { CHECK(!pthread_join(Threads[I],nullptr)); CHECK(!Jobs[I].Result); }
    checkpoint(B); _exit(0);
  }
  CHECK(waitpid(Child,&Status,0)==Child && WIFEXITED(Status) && !WEXITSTATUS(Status));
  puts("PASS(backend-v2): concurrent first use after fork registers sessions once");
  md_get_stats(B,&Stats); CHECK(Stats.bytes_in_use==0);
  printf("BACKEND_V2_END acquired=%llu released=%llu rejected=%llu live=%llu\n",
      (unsigned long long)Stats.acquire_successes,(unsigned long long)Stats.release_successes,
      (unsigned long long)Stats.kernel_rejected,(unsigned long long)Stats.bytes_in_use);
  md_backend_close(B);
  B=nullptr; CHECK(!md_backend_open(&B,&Config));
  CHECK(!md_acquire(B,65536,4096,Owner,&Small));
  CHECK(!md_release(B,&Small,0,nullptr)); checkpoint(B); md_backend_close(B);
  puts("PASS(backend-v2): same-mm close/reopen attaches same shared logs without reset/drain");
  puts("PASS(backend-v2): ALL integration tests");
}
