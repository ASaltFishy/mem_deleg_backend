#ifndef MD_LATENCY_PROFILE_H
#define MD_LATENCY_PROFILE_H
#ifndef MD_PROFILE_STAGE
#define MD_PROFILE_STAGE 0
#endif
#if MD_PROFILE_STAGE
#include <stdint.h>
namespace mdprof {
struct Buffer { uint64_t *Values; uint64_t Count; uint64_t Capacity; };
inline thread_local Buffer * volatile Current = nullptr;
inline uint64_t ticks() { uint64_t v; asm volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(v) :: "memory"); return v; }
inline uint64_t frequency() { uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
template<int Stage> struct Scope {
  Buffer *B = nullptr; uint64_t Start = 0;
  __attribute__((always_inline)) Scope() { if constexpr(Stage == MD_PROFILE_STAGE) { B=Current; if(B) Start=ticks(); } }
  __attribute__((always_inline)) void stop() { if constexpr(Stage == MD_PROFILE_STAGE) { if(B) { uint64_t end=ticks(); if(B->Count<B->Capacity) B->Values[B->Count++]=end-Start; B=nullptr; } } }
  __attribute__((always_inline)) ~Scope() { stop(); }
};
}
#define MD_SCOPE(ID, NAME) mdprof::Scope<ID> NAME
#define MD_STOP(NAME) NAME.stop()
#else
#define MD_SCOPE(ID, NAME)
#define MD_STOP(NAME)
#endif
#endif
