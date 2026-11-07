#ifndef AIMEE_DB1_PROCEDURE_EVENTS_H
#define AIMEE_DB1_PROCEDURE_EVENTS_H
/* Host-private DB1 interface. Learning must admit and project the event before
 * append. DB1 enforces immutable identity, bounded snapshots and CAS integrity.
 * JSON is owned, never passed through the 4096-byte interaction-event DTO. */
char *db1_procedure_event_load(const char *principal, const char *procedure_json);
/* 0 inserted, 1 identical duplicate, 2 snapshot changed/retry, -1 refused. */
int db1_procedure_event_append(const char *principal, const char *procedure_json,
                               const char *revision, const char *event_json,
                               const char *exposure_json, const char *scope_kind,
                               const char *scope_id);
#endif
