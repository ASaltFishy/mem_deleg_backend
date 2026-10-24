#ifndef SCUDO_DELEGATION_H_
#define SCUDO_DELEGATION_H_

#include "common.h"

namespace scudo {

struct ScudoDelegationLease {
  u32 BlockId = UINT32_MAX;
  u32 Owner = UINT32_MAX;
  u64 Generation = 0;
  u64 AllocationId = 0;
  u64 KernelSession = 0;
  uptr Base = 0;
  uptr Size = 0;
};

bool scudoDelegationEnabled();
bool scudoDelegationContains(uptr Address);
bool scudoDelegationAcquire(uptr Size, uptr Alignment, uptr HeadersSize,
                            ScudoDelegationLease &Lease,
                            uptr &EntryHeaderPosition);
bool scudoDelegationRelease(const ScudoDelegationLease &Lease);

} // namespace scudo

#endif
