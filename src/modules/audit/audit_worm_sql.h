/* Receipt lookup SQL shared by the WORM store and its plan test, so the test
 * checks the exact statement production runs. Private to src/modules/audit. */
#ifndef AIMEE_AUDIT_WORM_SQL_H
#define AIMEE_AUDIT_WORM_SQL_H

/* Receipt inspection has no time window, so without these a lookup scans the
 * whole ledger. Both are partial: only memory provider rows are indexed. The
 * CASE keeps a malformed detail from failing an append through the index. */
#define WORM_REQUEST_ID_EXPR                                                                       \
   "(CASE WHEN json_valid(detail) THEN json_extract(detail,'$.binding.request_id') END)"
#define WORM_LOOKUP_INDEX_SQL                                                                      \
   "CREATE INDEX IF NOT EXISTS audit_event_memory_request ON "                                     \
   "audit_event(actor_principal," WORM_REQUEST_ID_EXPR                                             \
   ") WHERE actor_role='host' AND action='memory.provider.prepared';"                              \
   "CREATE INDEX IF NOT EXISTS audit_event_memory_attempt ON audit_event(actor_principal,"         \
   "subject) WHERE actor_role='host' AND action LIKE 'memory.provider.%';"

/* Every recorded stage of the attempts prepared for one principal's request. */
#define WORM_READ_REQUEST_SQL                                                                      \
   "SELECT seq,event_id,action,subject,detail,row_hash FROM audit_event "                          \
   "WHERE seq<=?3 AND actor_role='host' AND actor_principal=?1 "                                   \
   "AND action LIKE 'memory.provider.%' "                                                          \
   "AND subject IN (SELECT subject FROM audit_event WHERE actor_role='host' "                      \
   "AND actor_principal=?1 AND action='memory.provider.prepared' "                                 \
   "AND " WORM_REQUEST_ID_EXPR "=?2) ORDER BY seq LIMIT 65"

/* Health reads one time window. Its seq range is the bound, so the principal
 * term is unary-plus'd: the receipt indexes above would otherwise win and read
 * every provider row the principal has ever written. */
#define WORM_HEALTH_REQUESTS_SQL                                                                   \
   "SELECT json_extract(detail,'$.binding.request_id') FROM audit_event "                          \
   "WHERE seq>=?5 AND seq<=?4 AND actor_role='host' AND +actor_principal=?1 "                      \
   "AND action='memory.provider.prepared' AND json_valid(detail) "                                 \
   "AND julianday(json_extract(detail,'$.at'))>=julianday(?2)-1.0/86400 "                          \
   "AND julianday(json_extract(detail,'$.at'))<=julianday(?3)+1.0/86400 "                          \
   "GROUP BY json_extract(detail,'$.binding.request_id') ORDER BY min(seq) DESC LIMIT 257"
#define WORM_HEALTH_ATTEMPTS_SQL                                                                   \
   "SELECT subject, json_extract(detail,'$.binding.request_id') "                                  \
   "FROM audit_event WHERE seq>=?2 AND seq<=?3 AND actor_role='host' "                             \
   "AND +actor_principal=?1 AND action='memory.provider.prepared' AND json_valid(detail)"
#define WORM_HEALTH_ROWS_SQL                                                                       \
   "SELECT seq,event_id,action,subject,detail,row_hash FROM audit_event "                          \
   "WHERE seq>=?2 AND seq<=?3 AND actor_role='host' AND +actor_principal=?1 "                      \
   "AND action LIKE 'memory.provider.%' ORDER BY seq"

#endif
