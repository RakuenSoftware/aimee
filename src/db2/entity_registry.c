/* entity_registry.c: surrogate-id entity canonicalization (typed-fact §3 / P2a).
 * See entity_registry.h. */
#include "../headers/aimee.h"
#include "entity_registry.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ER_ERRBUF  256
#define ER_NAME_MAX 256

/* Articles and honorifics are surface, not identity. "the Sunshine team" and
 * "Sunshine team" are one team; "Dr. Okafor" and "Okafor" are one person. Kept
 * as sorted-ish literal tables rather than a general stemmer because the set is
 * closed and the cost of a wrong fold is a merged entity. */
static int en_is_article(const char *t)
{
   return strcmp(t, "the") == 0 || strcmp(t, "a") == 0 || strcmp(t, "an") == 0;
}

/* Trailing ORGANISATIONAL descriptors: what someone calls the thing, not the
 * thing's name. "Sunshine", "Sunshine team" and "sunshine_team" are one client,
 * and filing them as three nodes is how a KB forgets what it was told.
 *
 * The list is deliberately narrow and the omissions are the point. "van",
 * "gateway", "router", "server", "box" and "project" are NOT here: "Girder
 * Gateway van" and "Ingot Router" carry those words INSIDE the product name, so
 * stripping them would merge distinct entities. The asymmetry drives the
 * caution -- a missed fold leaves two nodes and is recoverable by binding an
 * alias later, while a wrong fold silently welds two real entities together and
 * there is no evidence left to undo it from. */
static int en_is_org_descriptor(const char *t)
{
   static const char *const D[] = {"team",     "group",        "crew",     "squad",
                                   "dept",     "department",   "division", "account",
                                   "client",   "customer",     "contract", "org",
                                   "orgs",     "organisation", "organization"};
   for (size_t i = 0; i < sizeof(D) / sizeof(D[0]); i++)
      if (strcmp(t, D[i]) == 0)
         return 1;
   return 0;
}

static int en_is_honorific(const char *t)
{
   static const char *const H[] = {"dr",   "mr",        "mrs", "ms",  "miss",
                                   "prof", "professor", "sir", "rev"};
   for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++)
      if (strcmp(t, H[i]) == 0)
         return 1;
   return 0;
}

void entity_name_normalize(const char *in, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!in)
      return;

   /* Pass 1: casefold, and treat _ - / as word separators so "kb_server",
    * "kb-server" and "KB server" reach one node. This is the fold that was
    * missing: the benchmark's scorer has always applied it, which is why
    * extraction looked cleaner there than the graph actually was. */
   char buf[512];
   size_t o = 0;
   int pending_space = 0, started = 0;
   for (const char *p = in; *p && o + 1 < sizeof(buf); p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isspace(c) || c == '_' || c == '-' || c == '/')
      {
         if (started)
            pending_space = 1;
         continue;
      }
      if (pending_space && o + 1 < sizeof(buf))
      {
         buf[o++] = ' ';
         pending_space = 0;
      }
      buf[o++] = (char)tolower(c);
      started = 1;
   }
   buf[o] = '\0';

   /* Pass 2: per token, strip edge punctuation and TRAILING dots only, so a
    * sentence-final "Wellington." meets "Wellington" while 192.168.1.254 and
    * example.com keep their internal dots. */
   char *tok[64];
   int ntok = 0;
   for (char *t = strtok(buf, " "); t && ntok < 64; t = strtok(NULL, " "))
   {
      size_t len = strlen(t);
      while (len && strchr(",;:!?()[]{}\"'", t[len - 1]))
         t[--len] = '\0';
      while (len && t[len - 1] == '.')
         t[--len] = '\0';
      size_t lead = 0;
      while (t[lead] && strchr(",;:!?()[]{}\"'", t[lead]))
         lead++;
      if (t[lead])
         tok[ntok++] = t + lead;
   }

   /* Pass 3: drop leading articles/honorifics, but never everything -- an entity
    * legitimately named "A" must not normalise to the empty string and then
    * match every other empty name. */
   int first = 0;
   while (ntok - first > 1 && (en_is_article(tok[first]) || en_is_honorific(tok[first])))
      first++;
   /* And a trailing organisational descriptor, same "never strip to nothing"
    * rule: a team genuinely named "Team" keeps its name. */
   while (ntok - first > 1 && en_is_org_descriptor(tok[ntok - 1]))
      ntok--;

   size_t w = 0;
   for (int i = first; i < ntok; i++)
   {
      size_t len = strlen(tok[i]);
      if (w && w + 1 < out_len)
         out[w++] = ' ';
      if (w + len >= out_len)
         len = out_len - w - 1;
      memcpy(out + w, tok[i], len);
      w += len;
   }
   out[w] = '\0';
}

int64_t db2_entity_register(int kind, const char *status)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO entity_registry (kind, status) VALUES (?1, ?2)"
                        " RETURNING canonical_id",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", kind);
   aimee_pg_bind_text(st, "?2", (status && status[0]) ? status : ENTITY_STATUS_ACTIVE);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_entity_alias_bind(const char *name, int64_t canonical_id, int is_preferred)
{
   if (!name || !name[0] || canonical_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[256];
   entity_name_normalize(name, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   /* Reject a dangling alias: the target entity must exist (no FK on the sqlite
    * shim, so check explicitly — keeps the single-hop graph well-formed). */
   if (db2_entity_kind(canonical_id) < 0)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO entity_aliases (name, name_norm, canonical_id, is_preferred)"
                        " VALUES (?1, ?2, ?3, ?4) ON CONFLICT (name_norm) DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", norm);
   aimee_pg_bind_int64(st, "?3", canonical_id);
   aimee_pg_bind_int(st, "?4", is_preferred ? 1 : 0);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int64_t db2_entity_resolve(const char *name)
{
   if (!name || !name[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[256];
   entity_name_normalize(name, norm, sizeof(norm));
   if (!norm[0])
      return 0;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT a.canonical_id, r.status, r.merged_into FROM entity_aliases a"
                        " JOIN entity_registry r ON a.canonical_id = r.canonical_id"
                        " WHERE a.name_norm = ?1 AND a.suppressed = 0 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int64_t cid = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cid = aimee_pg_column_int64(st, 0);
      const char *status = aimee_pg_column_text(st, 1);
      int64_t merged_into = aimee_pg_column_int64(st, 2);
      /* Follow a merged row one hop (aliases are single-hop; merges are too). */
      if (status && strcmp(status, ENTITY_STATUS_MERGED) == 0 && merged_into > 0)
         cid = merged_into;
   }
   aimee_pg_finalize(st);
   return cid;
}

int64_t db2_entity_register_named(const char *name, int kind)
{
   if (!name || !name[0])
      return -1;
   int64_t existing = db2_entity_resolve(name);
   if (existing > 0)
      return existing;
   if (existing < 0)
      return -1; /* DB error, not "absent" */
   int64_t cid = db2_entity_register(kind, ENTITY_STATUS_ACTIVE);
   if (cid <= 0)
      return -1;
   if (db2_entity_alias_bind(name, cid, 1) != 0)
      return -1;
   /* Race guard: a concurrent caller may have bound this name to a *different*
    * entity first (our bind then no-ops via ON CONFLICT). Re-resolve; if the name
    * now resolves elsewhere, our just-created row is an orphan — drop it (no alias
    * points to it) and return the winner, so callers never reference an orphan. */
   int64_t winner = db2_entity_resolve(name);
   if (winner > 0 && winner != cid)
   {
      void *conn = db2_conn();
      if (conn)
      {
         char err[ER_ERRBUF] = "";
         aimee_pg_stmt_t *d =
             aimee_pg_prepare(conn,
                              "DELETE FROM entity_registry WHERE canonical_id = ?1"
                              " AND canonical_id NOT IN (SELECT canonical_id FROM entity_aliases)",
                              err, sizeof(err));
         if (d)
         {
            aimee_pg_bind_int64(d, "?1", cid);
            (void)aimee_pg_step(d, err, sizeof(err));
            aimee_pg_finalize(d);
         }
      }
      return winner;
   }
   return cid;
}

int db2_entity_mark_merged(int64_t from_id, int64_t into_id)
{
   if (from_id <= 0 || into_id <= 0 || from_id == into_id)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE entity_registry SET status = 'merged', merged_into = ?2 WHERE canonical_id = ?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", from_id);
   aimee_pg_bind_int64(st, "?2", into_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_entity_kind(int64_t canonical_id)
{
   if (canonical_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT kind FROM entity_registry WHERE canonical_id = ?1 LIMIT 1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", canonical_id);
   int kind = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      kind = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return kind;
}

int db2_entity_aliases_for(int64_t canonical_id, char (*out)[128], int max)
{
   if (canonical_id <= 0 || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT name FROM entity_aliases WHERE canonical_id = ?1 AND suppressed = 0"
                        " ORDER BY is_preferred DESC, id ASC LIMIT ?2",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", canonical_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *nm = aimee_pg_column_text(st, 0);
      snprintf(out[n], 128, "%s", nm ? nm : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int64_t db2_entity_merge(int64_t from_id, int64_t into_id)
{
   if (from_id <= 0 || into_id <= 0 || from_id == into_id)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   /* Atomic: a half-applied merge (registry flipped but no audit row, or vice
    * versa) would be unrecoverable, so everything rides one transaction. */
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   /* Both endpoints must currently be active. This rejects re-merging an
    * already-merged row and forecloses an A->B then B->A cycle: once A->B lands,
    * A is no longer active, so B->A's target fails this check. */
   int from_ok = 0, into_ok = 0;
   aimee_pg_stmt_t *cq = aimee_pg_prepare(conn,
                                          "SELECT canonical_id FROM entity_registry"
                                          " WHERE canonical_id IN (?1, ?2) AND status = 'active'",
                                          err, sizeof(err));
   if (cq)
   {
      aimee_pg_bind_int64(cq, "?1", from_id);
      aimee_pg_bind_int64(cq, "?2", into_id);
      while (aimee_pg_step(cq, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         int64_t got = aimee_pg_column_int64(cq, 0);
         if (got == from_id)
            from_ok = 1;
         else if (got == into_id)
            into_ok = 1;
      }
      aimee_pg_finalize(cq);
   }
   if (!from_ok || !into_ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   /* Audit row first so the registry row is never flipped to 'merged' without a
    * matching reversal record. */
   int64_t mid = -1;
   aimee_pg_stmt_t *ins = aimee_pg_prepare(
       conn, "INSERT INTO entity_merges (from_id, into_id) VALUES (?1, ?2) RETURNING id", err,
       sizeof(err));
   if (ins)
   {
      aimee_pg_bind_int64(ins, "?1", from_id);
      aimee_pg_bind_int64(ins, "?2", into_id);
      if (aimee_pg_step(ins, err, sizeof(err)) == AIMEE_PG_ROW)
         mid = aimee_pg_column_int64(ins, 0);
      aimee_pg_finalize(ins);
   }
   if (mid <= 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   /* Flip the registry row, still gated on 'active' (defends against a racing
    * concurrent merge that slipped between the check above and here). */
   int changed = 0;
   aimee_pg_stmt_t *up =
       aimee_pg_prepare(conn,
                        "UPDATE entity_registry SET status = 'merged', merged_into = ?2"
                        " WHERE canonical_id = ?1 AND status = 'active'",
                        err, sizeof(err));
   if (up)
   {
      aimee_pg_bind_int64(up, "?1", from_id);
      aimee_pg_bind_int64(up, "?2", into_id);
      if (aimee_pg_step(up, err, sizeof(err)) == AIMEE_PG_DONE)
         changed = aimee_pg_stmt_changes(up);
      aimee_pg_finalize(up);
   }
   if (changed != 1)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return mid;
}

int db2_entity_unmerge(int64_t merge_id)
{
   if (merge_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   /* Load the not-yet-undone merge record (both endpoints — into_id gates the
    * restore so we only reactivate a row still merged into THIS target). */
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn, "SELECT from_id, into_id FROM entity_merges WHERE id = ?1 AND undone = 0 LIMIT 1", err,
       sizeof(err));
   int64_t from_id = 0, into_id = 0;
   if (q)
   {
      aimee_pg_bind_int64(q, "?1", merge_id);
      if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         from_id = aimee_pg_column_int64(q, 0);
         into_id = aimee_pg_column_int64(q, 1);
      }
      aimee_pg_finalize(q);
   }
   if (from_id <= 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1; /* unknown id or already undone */
   }

   /* Restore to active ONLY if the row is still merged into exactly this target.
    * If it was since re-merged elsewhere (a different live merge), that newer
    * merge stays authoritative and this stale unmerge is refused. */
   int changed = 0;
   aimee_pg_stmt_t *u =
       aimee_pg_prepare(conn,
                        "UPDATE entity_registry SET status = 'active', merged_into = 0"
                        " WHERE canonical_id = ?1 AND merged_into = ?2 AND status = 'merged'",
                        err, sizeof(err));
   if (u)
   {
      aimee_pg_bind_int64(u, "?1", from_id);
      aimee_pg_bind_int64(u, "?2", into_id);
      if (aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE)
         changed = aimee_pg_stmt_changes(u);
      aimee_pg_finalize(u);
   }
   if (changed != 1)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1; /* registry state no longer matches the audit row */
   }

   int ok = 0;
   aimee_pg_stmt_t *m = aimee_pg_prepare(conn, "UPDATE entity_merges SET undone = 1 WHERE id = ?1",
                                         err, sizeof(err));
   if (m)
   {
      aimee_pg_bind_int64(m, "?1", merge_id);
      ok = (aimee_pg_step(m, err, sizeof(err)) == AIMEE_PG_DONE);
      aimee_pg_finalize(m);
   }
   if (!ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int64_t db2_entity_conflict_record(const char *name)
{
   if (!name || !name[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[256];
   entity_name_normalize(name, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   char err[ER_ERRBUF] = "";
   /* RETURNING id reads the (inserted or bumped) row's id in the same statement,
    * so there is no window where a concurrent delete could strand a stale id.
    * A repeat record only bumps priority — it never reopens a resolved row. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO entity_name_conflicts (name_norm, status, priority) VALUES (?1, 'open', 1)"
       " ON CONFLICT (name_norm) DO UPDATE SET priority = entity_name_conflicts.priority + 1"
       " RETURNING id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_entity_conflict_priority(const char *name)
{
   if (!name || !name[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[256];
   entity_name_normalize(name, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT priority FROM entity_name_conflicts WHERE name_norm = ?1 LIMIT 1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int p = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      p = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return p;
}

int db2_entity_conflict_set_status(int64_t conflict_id, const char *status)
{
   if (conflict_id <= 0 || !status || !status[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE entity_name_conflicts SET status = ?2 WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", conflict_id);
   aimee_pg_bind_text(st, "?2", status);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_entity_conflict_count(const char *status)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st;
   if (status && status[0])
   {
      st = aimee_pg_prepare(conn, "SELECT COUNT(*) FROM entity_name_conflicts WHERE status = ?1",
                            err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", status);
   }
   else
   {
      st = aimee_pg_prepare(conn, "SELECT COUNT(*) FROM entity_name_conflicts", err, sizeof(err));
      if (!st)
         return -1;
   }
   int c = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      c = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return c;
}

/* ── One-time re-normalisation of stored alias keys ──────────────────────────
 * entity_name_normalize() gained folds (separators, articles, honorifics,
 * trailing organisational descriptors) that earlier releases did not apply, so
 * rows written before this carry a name_norm computed the old way. Left alone
 * that is WORSE than the old behaviour rather than better: a lookup for
 * "kb_server" now normalises to "kb server", misses the stored "kb_server" row,
 * and creates a second entity for a name the registry already knew.
 *
 * So the stored keys are recomputed from the display name, once. Where two rows
 * now normalise the same way they were always the same entity and are merged
 * (db2_entity_merge, which resolve() follows) rather than one being dropped --
 * the edges pointing at the loser must keep resolving.
 *
 * Guarded by a kb_meta marker so it runs once per database. Idempotent anyway:
 * a second pass recomputes the same values and finds no collisions. */
int db2_entity_renormalize_aliases(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";

   aimee_pg_stmt_t *chk = aimee_pg_prepare(
       conn, "SELECT value FROM kb_meta WHERE key = 'entity_alias_norm_version'", err, sizeof(err));
   if (chk)
   {
      int done = (aimee_pg_step(chk, err, sizeof(err)) == AIMEE_PG_ROW);
      aimee_pg_finalize(chk);
      if (done)
         return 0;
   }

   aimee_pg_stmt_t *sel = aimee_pg_prepare(
       conn, "SELECT id, name, name_norm, canonical_id FROM entity_aliases ORDER BY id ASC", err,
       sizeof(err));
   if (!sel)
      return -1;

   struct
   {
      int64_t id, cid;
      char name[ER_NAME_MAX];
      char norm[ER_NAME_MAX];
   } *rows = NULL;
   size_t n = 0, cap = 0;
   while (aimee_pg_step(sel, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (n == cap)
      {
         size_t ncap = cap ? cap * 2 : 256;
         void *g = realloc(rows, ncap * sizeof(*rows));
         if (!g)
         {
            free(rows);
            aimee_pg_finalize(sel);
            return -1;
         }
         rows = g;
         cap = ncap;
      }
      rows[n].id = aimee_pg_column_int64(sel, 0);
      snprintf(rows[n].name, sizeof(rows[n].name), "%s", aimee_pg_column_text(sel, 1));
      snprintf(rows[n].norm, sizeof(rows[n].norm), "%s", aimee_pg_column_text(sel, 2));
      rows[n].cid = aimee_pg_column_int64(sel, 3);
      n++;
   }
   aimee_pg_finalize(sel);

   int changed = 0, merged = 0;
   for (size_t i = 0; i < n; i++)
   {
      char fresh[ER_NAME_MAX];
      entity_name_normalize(rows[i].name, fresh, sizeof(fresh));
      if (!fresh[0] || strcmp(fresh, rows[i].norm) == 0)
         continue;

      /* Does the new key already belong to someone? Then these were one entity
       * all along and the two registry rows are merged. */
      int64_t owner = db2_entity_resolve(fresh);
      if (owner > 0 && owner != rows[i].cid)
      {
         if (db2_entity_merge(rows[i].cid, owner) > 0)
            merged++;
         aimee_pg_stmt_t *d =
             aimee_pg_prepare(conn, "DELETE FROM entity_aliases WHERE id = ?1", err, sizeof(err));
         if (d)
         {
            aimee_pg_bind_int64(d, "?1", rows[i].id);
            (void)aimee_pg_step(d, err, sizeof(err));
            aimee_pg_finalize(d);
         }
         continue;
      }
      aimee_pg_stmt_t *u = aimee_pg_prepare(
          conn, "UPDATE entity_aliases SET name_norm = ?2 WHERE id = ?1", err, sizeof(err));
      if (!u)
         continue;
      aimee_pg_bind_int64(u, "?1", rows[i].id);
      aimee_pg_bind_text(u, "?2", fresh);
      if (aimee_pg_step(u, err, sizeof(err)) != AIMEE_PG_ERR)
         changed++;
      aimee_pg_finalize(u);
   }
   free(rows);

   aimee_pg_stmt_t *mark = aimee_pg_prepare(
       conn,
       "INSERT INTO kb_meta (key, value) VALUES ('entity_alias_norm_version', '2')"
       " ON CONFLICT (key) DO UPDATE SET value = '2'",
       err, sizeof(err));
   if (mark)
   {
      (void)aimee_pg_step(mark, err, sizeof(err));
      aimee_pg_finalize(mark);
   }
   if (changed || merged)
      fprintf(stderr, "aimee: entity aliases re-normalised: %d rekeyed, %d entities merged\n",
              changed, merged);
   return changed + merged;
}
