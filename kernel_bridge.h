#ifndef MEMORY_DELEGATION_KERNEL_BRIDGE_H
#define MEMORY_DELEGATION_KERNEL_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include "include/uapi/linux/memory_delegation.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct md_kernel_bridge md_kernel_bridge;
enum md_kernel_bridge_mode { MD_KERNEL_BRIDGE_OPTIONAL, MD_KERNEL_BRIDGE_REQUIRED };
typedef struct {
  uint32_t arena_cpu;
  uintptr_t arena_data_base;
  uint32_t arena_nr_pages;
  int mode;
  void *ring_memory;
  size_t ring_bytes;
} md_kernel_bridge_config;
// Broker initializes shared storage once, before publishing the domain.
int md_kernel_log_initialize(void *memory, size_t bytes, uint32_t pages);
int md_kernel_bridge_open(md_kernel_bridge **out, const md_kernel_bridge_config *config);
uint64_t md_kernel_bridge_session(const md_kernel_bridge *bridge);
// Caller holds Arena Mutex; holder may migrate. Owner-CPU FREE is reusable immediately; remote/migrated FREE waits.
int md_kernel_bridge_normal(md_kernel_bridge *bridge, const struct md_shared_record *record);
// Returns ticket+1 for migrated normal FREE confirmation; slot is kernel-owned
// after publication and may be reused immediately once normal.head advances.
int md_kernel_bridge_normal_ticket(md_kernel_bridge *bridge,
    const struct md_shared_record *record, uint64_t *ticket);
// Same normal-ring checks/publication, constructing ALLOC directly in the slot.
// Caller holds ArenaMutex; nothing is published if validation/capacity fails.
int md_kernel_bridge_alloc(md_kernel_bridge *bridge, uint64_t session,
                           uint16_t cpu, uint64_t block_id, uint64_t generation,
                           uint64_t allocation_id, uint32_t start_page,
                           uint32_t nr_pages);
// Remote producer touches only log storage; it never changes backend metadata.
enum { MD_USER_FREE_CLEAR = 1U }; // md_shared_slot.reserved: opaque userspace flags
int md_kernel_log_remote(struct md_shared_log *log, const struct md_shared_record *record,
                         unsigned user_flags);
// Release only the private handle. No drain, unpin, reset or shared-slot recycling.
void md_kernel_bridge_close(md_kernel_bridge *bridge);
#ifdef __cplusplus
}
#endif
#endif
