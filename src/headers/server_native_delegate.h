#ifndef AIMEE_SERVER_NATIVE_DELEGATE_H
#define AIMEE_SERVER_NATIVE_DELEGATE_H
#include <ouroboros/module_runtime.h>
#define SERVER_NATIVE_DELEGATE_EVENT     6679u
#define SERVER_NATIVE_DELEGATE_STAGE     23u
#define SERVER_NATIVE_DELEGATE_PRINCIPAL 76u
int server_native_delegate_start(void);
/* Private callback-local service ownership; never transport attestation. */
int server_native_delegate_active(void);
void server_native_delegate_stop(void);
ouro_module_status_t server_native_delegate_handle(const ouro_module_invocation_t *,
                                                   const uint8_t *, uint32_t, uint8_t *, uint32_t,
                                                   uint32_t *, void *);
#endif
