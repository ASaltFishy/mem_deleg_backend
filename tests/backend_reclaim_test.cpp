#include "delegation_backend.h"
#include "include/backend_broker.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#define CHECK(X) do { if (!(X)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#X,errno); abort(); } } while (0)
static std::atomic<int> Hook{0}, Entered{0}, Continue{0};
extern "C" int md_test_reclaim_hook(uintptr_t, size_t) {
  if (Hook == 1) { Entered = 1; while (!Continue.load()) usleep(100); }
  return Hook == 2 ? EIO : 0;
}
struct Job { md_backend *B; md_reclaim_policy P; md_reclaim_result R; int Error; };
static void *run(void *P) { auto *J=static_cast<Job*>(P); J->Error=md_backend_reclaim(J->B,&J->P,&J->R); return nullptr; }
static uint64_t blocks(md_backend *B) {
  int Fds[2]; CHECK(!md_backend_broker_export(B,Fds));
  struct stat S{}; CHECK(!fstat(Fds[1],&S)); close(Fds[0]);close(Fds[1]);
  return uint64_t(S.st_blocks)*512;
}
int main() {
  setbuf(stdout,nullptr);
  bool Kernel=getenv("EXPECT_KERNEL")!=nullptr;
  cpu_set_t Allowed; CHECK(!sched_getaffinity(0,sizeof(Allowed),&Allowed));
  unsigned Home=0; while(Home<CPU_SETSIZE&&!CPU_ISSET(Home,&Allowed)) ++Home;
  CHECK(Home<CPU_SETSIZE); cpu_set_t One; CPU_ZERO(&One);CPU_SET(Home,&One);
  CHECK(!sched_setaffinity(0,sizeof(One),&One));
  char Domain[96]; snprintf(Domain,sizeof(Domain),"md_reclaim_%d",getpid());
  if (!Kernel) { setenv("MD_KERNEL_BRIDGE","off",1);setenv("MD_SINGLE_ARENA","1",1); }
  const char *Shared=getenv("MEMORY_DELEGATION_DOMAIN");
  md_config Config{Shared?Shared:Domain,0x2000000000ULL,MD_DEFAULT_ARENA_CAPACITY,0};
  md_backend *B; CHECK(!md_backend_open(&B,&Config));
  md_stats Stats{};md_get_stats(B,&Stats);CHECK(!Kernel||Stats.kernel_enabled);
  if (getenv("MD_TEST_AUTOMATIC")) {
    md_lease A{}; CHECK(!md_acquire(B,1<<20,4096,md_process_id(),&A));
    memset((void*)A.address,0x5a,1<<20);uint64_t Before=blocks(B);
    uintptr_t Address=A.address;CHECK(!md_release(B,&A,0,nullptr));
    for(unsigned I=0;I<100&&blocks(B)>=Before;++I)usleep(50000);
    CHECK(blocks(B)<Before);
    // Backing release precedes re-publication; wait until the run is allocatable.
    for(unsigned I=0;I<100;++I) { md_get_stats(B,&Stats);if(Stats.bytes_free >= (1U<<20))break;usleep(1000); }
    CHECK(Stats.bytes_free >= (1U<<20));
    CHECK(!md_acquire(B,1<<20,4096,md_process_id(),&A));CHECK(A.address==Address);
    CHECK(*(unsigned char*)A.address==0);
    CHECK(!md_release(B,&A,0,nullptr));CHECK(!md_backend_checkpoint(B));
    md_backend_close(B);puts("RECLAIM_AUTOMATIC_PASS");return 0;
  }
  constexpr size_t Size=1<<20;
  md_lease A{},Live{},Other{};CHECK(!md_acquire(B,Size,4096,md_process_id(),&A));
  CHECK(!md_acquire(B,4096,4096,md_process_id(),&Live));
  memset((void*)A.address,0x5a,Size);memset((void*)Live.address,0x6b,4096);
  uint64_t Before=blocks(B);CHECK(!md_release(B,&A,0,nullptr));
  CHECK(!md_backend_checkpoint(B));
  Job J{B,{0,0,0,Size,8},{},0}; Hook=1;
  pthread_t T;CHECK(!pthread_create(&T,nullptr,run,&J));
  for(unsigned I=0;!Entered&&I<10000;++I)usleep(100);
  CHECK(Entered);
  CHECK(!md_acquire(B,4096,4096,md_process_id(),&Other));
  CHECK(Other.address<A.address||Other.address>=A.address+Size);
  memset((void*)Other.address,0x7c,4096);
  Continue=1;CHECK(!pthread_join(T,nullptr));Hook=0;
  CHECK(!J.Error&&J.R.bytes_reclaimed==Size&&!J.R.failures);
  CHECK(blocks(B)+Size<=Before+4096);
  CHECK(*(unsigned char*)Live.address==0x6b&&*(unsigned char*)Other.address==0x7c);
  CHECK(!md_backend_reclaim(B,&J.P,&J.R));CHECK(!J.R.bytes_reclaimed);
  CHECK(!md_acquire(B,Size,4096,md_process_id(),&A));
  CHECK(*(unsigned char*)A.address==0&&*((unsigned char*)A.address+Size-1)==0);
  memset((void*)A.address,0x33,Size);
  puts("PASS reclaim: backing released, VA reused/zero-filled, live and concurrent allocations protected, cold skipped");
  CHECK(!md_release(B,&A,0,nullptr));CHECK(!md_backend_checkpoint(B));Hook=2;
  CHECK(!md_backend_reclaim(B,&J.P,&J.R));CHECK(J.R.failures&&J.R.last_error==EIO&&!J.R.bytes_reclaimed);Hook=0;
  CHECK(!md_acquire(B,Size,4096,md_process_id(),&A));CHECK(*(unsigned char*)A.address==0x33);
  CHECK(!md_release(B,&A,0,nullptr));CHECK(!md_backend_checkpoint(B));
  J.P={UINT64_MAX,UINT64_MAX,100000000ULL,Size,8};
  CHECK(!md_backend_reclaim(B,&J.P,&J.R));CHECK(!J.R.bytes_reclaimed);
  usleep(150000);CHECK(!md_backend_reclaim(B,&J.P,&J.R));CHECK(J.R.bytes_reclaimed==Size);
  puts("PASS reclaim: failure rollback and TTL without budget pressure");
  CHECK(!md_release(B,&Live,0,nullptr));CHECK(!md_release(B,&Other,0,nullptr));CHECK(!md_backend_checkpoint(B));
  // Contiguous hot/cold runs must still satisfy a single larger allocation.
  CHECK(!md_acquire(B,Size+8192,4096,md_process_id(),&A));
  CHECK(A.address==Config.fixed_address+Home*(Config.capacity+4096)||!Kernel);
  CHECK(!md_release(B,&A,0,nullptr));CHECK(!md_backend_checkpoint(B));
  J.P={0,0,0,4096,1};CHECK(!md_backend_reclaim(B,&J.P,&J.R));CHECK(J.R.bytes_reclaimed==4096&&J.R.runs_reclaimed==1);
  CHECK(!md_backend_reclaim(B,&J.P,&J.R));CHECK(J.R.bytes_reclaimed==4096);
  puts("PASS reclaim: mixed-temperature coalescing and bounded batches make progress");
  md_backend_close(B);
  if(!Shared) { char Name[128];snprintf(Name,sizeof(Name),"/%s_meta",Domain);shm_unlink(Name);snprintf(Name,sizeof(Name),"/%s_data",Domain);shm_unlink(Name); }
  puts("RECLAIM_TEST_PASS");
}
