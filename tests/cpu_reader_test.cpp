#include "delegation_backend.h"
#include <pthread.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(X) do { if(!(X)) {fprintf(stderr,"CPU_READER_FAIL line=%d %s\n",__LINE__,#X);exit(1);} } while(0)
static int Home=-1,Other=-1;
static void pin(int Cpu) {cpu_set_t Set;CPU_ZERO(&Set);CPU_SET(Cpu,&Set);CHECK(!sched_setaffinity(0,sizeof(Set),&Set));}
static void compare() { CHECK(md_cpu_current()==sched_getcpu()); }
static void *thread(void*) {
 for(int i=0;i<16;++i) {pin(i&1?Home:Other);compare();}
 return nullptr;
}
int main() {
 cpu_set_t Set;CHECK(!sched_getaffinity(0,sizeof(Set),&Set));
 for(int c=0;c<CPU_SETSIZE;++c) if(CPU_ISSET(c,&Set)) {if(Home<0)Home=c;else if(Other<0)Other=c;}
 CHECK(Other>=0);pin(Home);compare();
 const char *Mode=md_cpu_query_mode();
 printf("CPU_READER_MODE=%s\n",Mode);
 if(getenv("EXPECT_RSEQ")) CHECK(!strncmp(Mode,"rseq_",5));
 for(int i=0;i<32;++i) {pin(i&1?Home:Other);compare();}
 pid_t Child=fork();CHECK(Child>=0);
 if(!Child) {pin(Other);compare();pin(Home);compare();_exit(0);}
 int Status;CHECK(waitpid(Child,&Status,0)==Child && WIFEXITED(Status) && !WEXITSTATUS(Status));
 // Exercise registration/destruction and TLS address reuse across thread exit.
 for(int round=0;round<8;++round) {
  pthread_t T[4];for(auto &t:T)CHECK(!pthread_create(&t,nullptr,thread,nullptr));
  for(auto &t:T)CHECK(!pthread_join(t,nullptr));
 }
 pin(Home);compare();
 puts("CPU_READER_ALL_PASS migration/fork/thread-TLS-reuse");
}
