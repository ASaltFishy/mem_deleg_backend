#ifndef MD_BACKEND_BROKER_PROTOCOL_H
#define MD_BACKEND_BROKER_PROTOCOL_H
#include <stdint.h>
#define MD_BROKER_MAGIC 0x4d444236U
#define MD_BROKER_VERSION 8U
struct md_broker_request {
  uint32_t magic, version, page_size, arena_count;
  uint64_t capacity, fixed_address;
  char domain[128];
};
struct md_broker_response { uint32_t magic, version; int32_t error; uint32_t reserved; };
#endif
