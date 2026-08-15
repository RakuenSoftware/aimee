/* guardrail_events.h: DB1 storage for semantic guardrail event records. */
#ifndef DEC_GUARDRAIL_EVENTS_H
#define DEC_GUARDRAIL_EVENTS_H 1

typedef struct
{
   char session_id[64];
   char tool_name[64];
   double overall_risk;
   double action_risk;
   double diff_risk;
   double drift_risk;
   double antipattern_similarity;
   char recommendation[16];
   char labels[256];
   char final_action[16];
   char explanation[512];
   int dry_run;
} guardrail_event_t;

typedef struct
{
   int dry_run;
   int warn;
   int prompt;
   int block;
} guardrail_event_counts_t;

typedef struct
{
   int id;
   char recorded_at[32];
   char session_id[64];
   char tool_name[64];
   double overall_risk;
   char labels[256];
   char final_action[16];
   char explanation[512];
   int dry_run;
} guardrail_event_row_t;

int db1_guardrail_event_insert(const guardrail_event_t *e);
int db1_guardrail_event_counts_7d(guardrail_event_counts_t *out);
int db1_guardrail_event_session_advisory_count(const char *session_id, int *out);
int db1_guardrail_event_list(int limit, int only_advisory, guardrail_event_row_t *rows, int *count);

#endif /* DEC_GUARDRAIL_EVENTS_H */
