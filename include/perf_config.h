#ifndef MD_PERF_CONFIG_H
#define MD_PERF_CONFIG_H
#ifndef MD_PERF_MINIMAL
#define MD_PERF_MINIMAL 0
#endif
#if MD_PERF_MINIMAL
#undef MD_PROFILE_STAGE
#define MD_PROFILE_STAGE 0
#undef MD_SLOW_TRACE
#define MD_SLOW_TRACE 0
#undef MD_TOUCH_TRACE
#define MD_TOUCH_TRACE 0
#undef SCUDO_SECONDARY_STATS
#define SCUDO_SECONDARY_STATS 0
#define MD_STAT(...) do { } while (0)
#else
#define MD_STAT(...) do { __VA_ARGS__; } while (0)
#endif
#endif
