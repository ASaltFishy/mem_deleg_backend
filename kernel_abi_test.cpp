#include "kernel_bridge.h"
#include "include/memory_delegation_prctl.h"
#include "include/uapi/linux/memory_delegation.h"

#include <cstddef>
#include <cstdint>

static_assert(static_cast<unsigned>(MD_LOG_ALLOC) ==
                  static_cast<unsigned>(MD_KERNEL_LOG_ALLOC),
              "ALLOC opcode drift");
static_assert(static_cast<unsigned>(MD_LOG_FREE) ==
                  static_cast<unsigned>(MD_KERNEL_LOG_FREE),
              "FREE opcode drift");
static_assert(MD_LOG_F_PREFAULT == MD_KERNEL_LOG_PREFAULT,
              "prefault flag drift");
static_assert(sizeof(md_shadow_log) == 12, "shadow-log size drift");
static_assert(offsetof(md_log_ring, entries) == 36,
              "log-ring header drift");
static_assert(PR_SET_MEMORY_DELEGATION_LOG == 74,
              "prctl option drift");
static_assert(PR_MEMORY_DELEGATION_F_RESET_ARENA == (1UL << 0),
              "reset flag drift");
static_assert(PR_MEMORY_DELEGATION_F_DRAIN_LOG == (1UL << 1),
              "drain flag drift");

int main() { return 0; }
