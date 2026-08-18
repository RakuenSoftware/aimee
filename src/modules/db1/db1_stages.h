/* Entry points for the generated DB1 stage handlers.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
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

#endif /* AIMEE_DB1_STAGES_H */
/* clang-format on */
