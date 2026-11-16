#ifndef AIMEE_OBS_BUS_CALL_H
#define AIMEE_OBS_BUS_CALL_H
#include <ouroboros/module_client.h>
/* Host thread-cancellation ownership around the unchanged ring transport. */
ouro_module_call_result_t aimee_obs_module_pool_call(
    ouro_module_client_pool_t *pool, uint32_t kind, uint32_t stage, uint64_t trace,
    uint64_t deadline, const void *request, uint32_t request_len, void *response, uint32_t capacity,
    uint32_t *response_len, ouro_module_cancelled_fn cancelled, void *cancel_context);
#endif
