#ifndef MD_BACKEND_BROKER_H
#define MD_BACKEND_BROKER_H
#include "delegation_backend.h"
#ifdef __cplusplus
extern "C" {
#endif
// Service-only creation. Ordinary Android backend_open can only attach.
int md_backend_broker_create(md_backend **out, const md_config *config);
int md_backend_broker_export(md_backend *backend, int fds[2]);
#ifdef __cplusplus
}
#endif
#endif
