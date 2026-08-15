/* interaction_events.h: DB1 canonical interaction event stream. */
#ifndef DEC_DB1_INTERACTION_EVENTS_H
#define DEC_DB1_INTERACTION_EVENTS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      IE_USER_TURN,
      IE_AGENT_TURN,
      IE_TOOL_CALL,
      IE_TOOL_OUTCOME,
      IE_DELEGATE_EXIT,
      IE_GUARDRAIL_DECISION,
      IE_SKILL_ACTIVATION,
      IE_USER_CORRECTION,
      IE_FAILOVER_EVENT,
      IE_MCP_PACKAGE_CHECK,
   } ie_event_type_t;

   typedef struct
   {
      int id;
      char created_at[32];
      char session_id[128];
      char event_type[32];
      char actor[16];
      char payload[4096];
      char outcome[16];
   } ie_event_row_t;

   const char *ie_event_type_name(ie_event_type_t type);

   int ie_record(const char *session_id, ie_event_type_t type, const char *actor,
                 const char *payload_json, const char *outcome);

   int ie_list_unreflected_for_session(const char *session_id, ie_event_row_t *out, int max);
   int ie_list_for_session(const char *session_id, ie_event_row_t *out, int max);
   int ie_mark_reflected(const int *ids, int count);
   int ie_list_promotion_feed(ie_event_row_t *out, int max);
   int ie_mark_promoted(const int *ids, int count);

   int interaction_events_evict_if_needed(int cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_INTERACTION_EVENTS_H */
