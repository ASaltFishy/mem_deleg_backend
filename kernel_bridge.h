#ifndef MEMORY_DELEGATION_KERNEL_BRIDGE_H
#define MEMORY_DELEGATION_KERNEL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_kernel_bridge md_kernel_bridge;

enum md_kernel_bridge_mode {
  MD_KERNEL_BRIDGE_OPTIONAL = 0,
  MD_KERNEL_BRIDGE_REQUIRED = 1,
};

typedef struct {
  uint32_t arena_cpu;
  uintptr_t arena_data_base;
  uint32_t arena_nr_pages;
  int reset_arena;
  int mode;
  void *ring_memory; // Optional shared, page-aligned storage, retained by caller.
  uint32_t channel_id;
} md_kernel_bridge_config;

enum md_kernel_log_operation {
  MD_KERNEL_LOG_ALLOC = 1,
  MD_KERNEL_LOG_FREE = 2,
};

enum { MD_KERNEL_LOG_PREFAULT = 1U << 0 };

// Opens one per-(mm, arena CPU) ownership-log channel using the external
// backend ABI. In OPTIONAL mode an unsupported kernel succeeds with
// *out == NULL; REQUIRED mode returns ENOTSUP.
int md_kernel_bridge_open(md_kernel_bridge **out,
                          const md_kernel_bridge_config *config);

// Appends only: successful return means enqueued, not kernel-acknowledged.
// FREE must remain Pending until the shared ring's head passes its ticket.
// Ring-full backpressure yields then drains only if still full; errors stick.
int md_kernel_bridge_enqueue(md_kernel_bridge *bridge, unsigned operation,
                            uint32_t start_page, uint32_t nr_pages,
                            unsigned flags, uint32_t *ticket);
uint32_t md_kernel_bridge_channel(const md_kernel_bridge *bridge);

int md_kernel_bridge_transition(md_kernel_bridge *bridge, unsigned operation,
                                uint32_t start_page, uint32_t nr_pages,
                                unsigned flags);

int md_kernel_bridge_drain(md_kernel_bridge *bridge);
uint32_t md_kernel_bridge_pending(md_kernel_bridge *bridge);
void md_kernel_bridge_close(md_kernel_bridge *bridge);

#ifdef __cplusplus
}
#endif

#endif
