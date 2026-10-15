#ifndef MD_JEMALLOC_ADAPTER_H
#define MD_JEMALLOC_ADAPTER_H

#include "delegation_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

int md_jemalloc_init(const char *domain, unsigned *arena);
void md_jemalloc_stats(md_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
