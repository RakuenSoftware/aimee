/* learning_evidence.c: evidence capture into the charter artifact pipeline.
 * See docs/proposals/done/cross-source-learning-substrate.md */

#include "learning_evidence.h"
#include "module_commands.h"
#include "aimee.h" /* memory_directive_t and friends, TIER_* */
#include "modules/kb/c/artifacts.h"
#include "modules/kb/c/anti_patterns.h"
#include "modules/kb/c/workflow_patterns.h"
#include "modules/kb/c/rules.h"
#include "modules/kb/c/entity_nodes.h"
#include "modules/kb/c/evidence_vectors.h"
#include "modules/kb/c/learning_synth_ops.h"
#include "modules/kb/c/kb_service_backend.h"
#include "modules/kb/c/demotion.h"
#include "log.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Borderline window below the promotion threshold: candidates here still
 * auto-apply but the audit row is flagged_for_review. */
#define LEARNING_PROMOTE_BORDERLINE 0.10

/* Build a minimal JSON payload for a feedback artifact. */
static void build_feedback_payload(const char *polarity, const char *title, const char *description,
                                   char *buf, size_t len)
{
   const char *desc = description ? description : "";
   snprintf(buf, len, "{\"polarity\":\"%s\",\"title\":\"%s\",\"description\":\"%s\"}",
            polarity ? polarity : "", title ? title : "", desc);
}

/* The working-profile field writer lives in DB1 (db1/working_profile_local.c).
 * Declared weak so this object links into the DB1-less aimee-kb binary; there
 * the symbol resolves to NULL and working_profile promotion no-ops (it only runs
 * in the DB1-owning process). The full binary and unit tests provide it. */
extern int db1_working_profile_local_observe(const char *field, const char *value,
                                             double confidence, const char *session_id,
                                             int threshold) __attribute__((weak));

int learning_evidence_write_feedback(const char *polarity, const char *title,
                                     const char *description, const char *operator_id,
                                     char *artifact_id_out, int artifact_id_out_len)
{
   if (artifact_id_out && artifact_id_out_len > 0)
      artifact_id_out[0] = '\0';

   if (!polarity || !title)
      return -1;

   const char *kind =
       (strcmp(polarity, "positive") == 0) ? "feedback_positive" : "feedback_negative";

   char id[64];
   kb_store_artifact_gen_id(id, sizeof(id));

   char payload[512];
   build_feedback_payload(polarity, title, description, payload, sizeof(payload));

   int rc = kb_store_artifact_write(id, kind, "proposed", "user", operator_id ? operator_id : "",
                                    operator_id ? operator_id : "", 1.0, payload);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "feedback artifact write failed (kind=%s)", kind);
      return -1;
   }

   if (artifact_id_out && artifact_id_out_len > 0)
      snprintf(artifact_id_out, (size_t)artifact_id_out_len, "%s", id);

   aimee_log(LOG_DEBUG, "learning_evidence", "feedback evidence artifact: id=%s kind=%s", id, kind);
   return 0;
}

int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                            const int64_t *surfaced_ids, int n_surfaced,
                                            char *id_out, int id_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';

   int rc = kb_store_demotion_retrieval_event_write(query_fingerprint, role, surfaced_ids,
                                                    n_surfaced, id_out, id_out_len);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "retrieval_event write failed");
      return -1;
   }
   return 0;
}

int learning_evidence_write_retrieval_attribution(const char *retrieval_event_id,
                                                  int64_t surfaced_row_id, const char *verdict,
                                                  double weight)
{
   if (!retrieval_event_id || !retrieval_event_id[0])
      return -1;

   int rc = kb_store_demotion_retrieval_attribution_write(retrieval_event_id, surfaced_row_id,
                                                          verdict, weight);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "retrieval_attribution write failed (row=%lld)",
                (long long)surfaced_row_id);
      return -1;
   }
   return 0;
}
