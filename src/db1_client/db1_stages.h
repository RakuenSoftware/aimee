/* Entry points for the generated DB1 stage handlers.
 *
 * WAS GENERATED from the store catalog by scripts/gen_db1_contract.py. Both
 * moved on: the catalog is now server-go/modules/aimee/operations.json, and the
 * generator was deleted with the C module.
 *
 * So this is maintained BY HAND now, and the header used to say "Do not edit"
 * while pointing at a generator that no longer exists and a path that no longer
 * resolves -- which is a dead end at exactly the moment someone needs to change
 * something. Edit it, and keep it agreeing with the catalog:
 * scripts/check-db1-client-contract.py matches every call site here against the
 * catalog by arity and reply width, and runs in lint on every pull request.
 * That check is what replaced the generator.
 *
 * module_adapter.c dispatches by stage and calls these; each is emitted beside
 * the domain it serves.
 *
 * clang-format is off below: the canonical form is whatever this emits. */
/* clang-format off */
#ifndef AIMEE_DB1_STAGES_H
#define AIMEE_DB1_STAGES_H 1

#include <aimee/core/event_bus/module_runtime.h>

#include <stdint.h>

aimee_module_status_t aimee_db1_stage_git_ownership(const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body,
                                           uint32_t response_capacity,
                                           uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_conversation(const uint8_t *request_body, uint32_t request_len,
                                          uint8_t *response_body,
                                          uint32_t response_capacity,
                                          uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_agent_work(const uint8_t *request_body, uint32_t request_len,
                                        uint8_t *response_body,
                                        uint32_t response_capacity,
                                        uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_delegation(const uint8_t *request_body, uint32_t request_len,
                                        uint8_t *response_body,
                                        uint32_t response_capacity,
                                        uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_sessions(const uint8_t *request_body, uint32_t request_len,
                                      uint8_t *response_body,
                                      uint32_t response_capacity,
                                      uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_runtime(const uint8_t *request_body, uint32_t request_len,
                                     uint8_t *response_body,
                                     uint32_t response_capacity,
                                     uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_telemetry(const uint8_t *request_body, uint32_t request_len,
                                       uint8_t *response_body,
                                       uint32_t response_capacity,
                                       uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_guardrail_state(const uint8_t *request_body, uint32_t request_len,
                                             uint8_t *response_body,
                                             uint32_t response_capacity,
                                             uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_ensemble(const uint8_t *request_body, uint32_t request_len,
                                      uint8_t *response_body,
                                      uint32_t response_capacity,
                                      uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_workflow(const uint8_t *request_body, uint32_t request_len,
                                      uint8_t *response_body,
                                      uint32_t response_capacity,
                                      uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_roundtable(const uint8_t *request_body, uint32_t request_len,
                                        uint8_t *response_body,
                                        uint32_t response_capacity,
                                        uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_identity(const uint8_t *request_body, uint32_t request_len,
                                      uint8_t *response_body,
                                      uint32_t response_capacity,
                                      uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_checkpoints(const uint8_t *request_body, uint32_t request_len,
                                         uint8_t *response_body,
                                         uint32_t response_capacity,
                                         uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_jti_replay(const uint8_t *request_body, uint32_t request_len,
                                        uint8_t *response_body,
                                        uint32_t response_capacity,
                                        uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_lifecycle(const uint8_t *request_body, uint32_t request_len,
                                       uint8_t *response_body,
                                       uint32_t response_capacity,
                                       uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_mgmt_jwks(const uint8_t *request_body, uint32_t request_len,
                                       uint8_t *response_body,
                                       uint32_t response_capacity,
                                       uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_mgmt_nonce(const uint8_t *request_body, uint32_t request_len,
                                        uint8_t *response_body,
                                        uint32_t response_capacity,
                                        uint32_t *response_len);

aimee_module_status_t aimee_db1_stage_pki(const uint8_t *request_body, uint32_t request_len,
                                 uint8_t *response_body,
                                 uint32_t response_capacity,
                                 uint32_t *response_len);

#endif /* AIMEE_DB1_STAGES_H */
/* clang-format on */
