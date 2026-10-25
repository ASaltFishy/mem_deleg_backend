#ifndef ALLOCATOR_DELEGATION_BACKEND_H
#define ALLOCATOR_DELEGATION_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_backend md_backend;

// Payload capacity per CPU arena; metadata and guard pages are separate.
// A bounded shared pool: live blocks, headers, alignment and fragmentation all
// consume capacity. Failed acquisitions return to the allocator native path.
// This reserves address space, not resident payload.
// All clients and the broker must agree; change backing only after reboot.
#define MD_DEFAULT_ARENA_CAPACITY (256ULL << 20)

typedef struct {
  const char *domain;
  uintptr_t fixed_address;
  // Capacity per CPU arena. Arenas have one unmapped guard page between them
  // so the kernel can attach an independent ownership VMA to each arena.
  size_t capacity;
  int reset;
} md_config;

typedef struct {
  uint32_t block_id;
  uint32_t owner;
  uint64_t generation;
  uintptr_t address;
  size_t size;
  uint64_t allocation_id;
  uint64_t kernel_session;
} md_lease;

typedef struct {
  uint32_t block_id;
  uint64_t generation;
} md_candidate;

typedef struct {
  uint32_t arena;
  uintptr_t data_base;
  size_t data_size;
  uint32_t nr_pages;
} md_arena_layout;

typedef struct {
  uint64_t acquire_calls;
  uint64_t acquire_successes;
  uint64_t release_calls;
  uint64_t release_successes;
  uint64_t split_count;
  uint64_t merge_count;
  uint64_t bytes_in_use;
  uint64_t bytes_free;
  uint64_t lock_acquisitions;
  uint64_t lock_contentions;
  uint64_t lock_wait_ns;
  uint64_t remote_enqueued;
  uint64_t remote_drained;
  uint64_t rseq_successes;
  uint64_t rseq_aborts;
  uint64_t descriptor_pops;
  uint64_t address_steps;
  uint64_t hint_hits;
  uint64_t queue_contentions;
  uint64_t queue_wait_ns;
  uint64_t kernel_rejected;
  uint64_t kernel_enabled;
} md_stats;

// CPU IDs are Linux logical IDs; benchmarks select one sibling per core.
// Read kernel-maintained rseq CPU ID in userspace; safe syscall fallback.
int md_cpu_current(void);
const char *md_cpu_query_mode(void);
int md_current_arena(md_backend *backend);
unsigned md_arena_count(md_backend *backend);
int md_get_arena_layout(md_backend *backend, unsigned arena,
                        md_arena_layout *out);
int md_drain_remote(md_backend *backend, unsigned arena);

int md_backend_open(md_backend **out, const md_config *config);
void md_backend_close(md_backend *backend);
// Administrative checkpoint: caller pauses producers; schedules owner CPUs and
// reaps receipts under their Arena Mutex. No explicit kernel drain/reset.
// Never call from the measured allocation fast path.
int md_backend_checkpoint(md_backend *backend);
uint32_t md_process_id(void);

int md_acquire(md_backend *backend, size_t size, size_t alignment,
               uint32_t owner, md_lease *out);
// A lease covers entirely readable/writable pages. Layout is allocator-owned.
enum { MD_ZERO = 1 };
int md_acquire_flags(md_backend *backend, size_t size, size_t alignment,
                     uint32_t owner, unsigned flags, md_lease *out);
int md_try_acquire(md_backend *backend, md_candidate candidate,
                   uint32_t owner, md_lease *out);
int md_release(md_backend *backend, const md_lease *lease, int clear_payload,
               md_candidate *out_candidate);
// Kernel-enabled or remote release queues publication and returns an invalid
// {UINT32_MAX, 0} candidate. Reuse waits for kernel completion when enabled.
// md_get_stats drains mailboxes; it is an administrative, not hot-path API.
int md_release_address(md_backend *backend, uintptr_t address, size_t size,
                       uint32_t owner, int clear_payload);

int md_contains(const md_backend *backend, uintptr_t address);
int md_lease_for_address(md_backend *backend, uintptr_t address,
                         uint32_t owner, md_lease *out);
void md_get_stats(md_backend *backend, md_stats *out);

#ifdef __cplusplus
}
#endif

#endif
