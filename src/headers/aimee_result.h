/* aimee_result.h: the integer a function in this project answers with.
 *
 * See docs/proposals/pending/project-result-code-convention.md.
 *
 *     < 0   failed, and the specific value says which failure
 *     = 0   not finished: running, queued, in flight -- no outcome yet
 *     > 0   succeeded, and the specific value says which success
 *
 * Every function in this project whose integer return is how the caller learns
 * what happened -- not one subsystem's house style.
 *
 * UNLESS another standard already owns that number. Where the value is defined
 * outside this project and read by something outside it, that standard wins and
 * this one does not apply: HTTP status codes, POSIX errno, SQLSTATE, process
 * exit status, signal numbers, and protocol wire statuses. The test is not "is
 * this number small" but "did someone else define it". A function translating
 * between the two worlds translates, and its own return follows this convention
 * even when its argument does not.
 *
 * Three properties, each doing work:
 *
 * Zero is not an outcome. It means the statement is still running, the job is
 * still queued, the process is alive with nothing decided. A caller holding a
 * zero has not been told the result, because there is not one. This inverts the
 * practice it replaces, where zero was the usual way to say "fine".
 *
 * A success that changed nothing is still a success, with its own number. An
 * upsert whose row already held what it would have written, a delete of
 * something already gone, an update whose WHERE matched nothing: all three ran
 * correctly. They are AIMEE_DONE_NO_CHANGE or AIMEE_DONE_ALREADY, never zero.
 *
 * A determination is a success. An operation asked to decide something, which
 * decides no, has succeeded: that is AIMEE_DONE_REFUSED. Only a refusal that
 * stopped the operation running -- an authorization check the caller failed --
 * is AIMEE_DENIED and negative.
 *
 * Sign alone was never enough. -1 appears in thirty-five headers meaning
 * thirty-five things, so a caller receiving one has learned only that something
 * went wrong. The specific number is the point; the sign is how it is read
 * without a table.
 *
 * This is NOT the module transport status. AIMEE_MODULE_STATUS_* and
 * bus.ModuleStatus answer whether the call happened, on an unsigned envelope
 * field, and stay separate: an operation that runs and fails answers
 * AIMEE_MODULE_STATUS_OK with a negative result in its body. The call
 * succeeded; the work did not.
 *
 * Adoption is not a flag day. A converted function and an unconverted one can
 * call each other: -1 still means failure and a positive still means success.
 * The only value whose meaning actually moves is zero. */
#ifndef DEC_AIMEE_RESULT_H
#define DEC_AIMEE_RESULT_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Reserved project-wide. Never a domain code, never a success, never an
    * error: the work has not finished. */
#define AIMEE_PENDING 0

   /* --- universal successes, 1..99 ------------------------------------- */

   /* Completed; the effect was applied. */
#define AIMEE_DONE 1
   /* Completed; the statement ran and nothing needed changing. An UPDATE whose
    * WHERE matched no rows is this. */
#define AIMEE_DONE_NO_CHANGE 2
   /* Completed; it was already so before the call, so nothing needed to run.
    * An ON CONFLICT DO NOTHING that conflicted is this, and it is NOT
    * AIMEE_DONE_NO_CHANGE -- one write matched nothing, the other never ran. */
#define AIMEE_DONE_ALREADY 3
   /* Completed for part of the work; the remainder is reported alongside and is
    * not an error. */
#define AIMEE_DONE_PARTIAL 4
   /* Completed; the answer is legitimately nothing. A read that found no rows
    * is this, and it exists so that answer stops being indistinguishable from a
    * read that could not run. */
#define AIMEE_DONE_EMPTY 5
   /* Completed; the decision this was asked to make is no. A policy verdict, an
    * ontology check, a gate that ran and declined. */
#define AIMEE_DONE_REFUSED 6

   /* --- universal failures, -1..-99 ------------------------------------ */

   /* Unspecified. Permitted only where nothing more is known -- a function
    * still answering this is unfinished, not wrong. */
#define AIMEE_FAILED -1
   /* The request was malformed, out of range, or incoherent. */
#define AIMEE_INVALID -2
   /* Authorization refused the caller. Distinct from AIMEE_DONE_REFUSED: this
    * one stopped the operation from running. */
#define AIMEE_DENIED -3
   /* There was no verified principal to authorize. */
#define AIMEE_UNAUTHENTICATED -4
   /* A dependency was absent: no connection, no installed provider. */
#define AIMEE_UNAVAILABLE -5
   /* Lost a race, or a precondition moved underneath. */
#define AIMEE_CONFLICT -6
   /* The named thing does not exist AND its absence is an error. Where absence
    * is the answer, that is AIMEE_DONE_EMPTY and positive; both exist so the
    * two stop being one. */
#define AIMEE_NOT_FOUND -7
   /* The input or the answer exceeds a stated bound. */
#define AIMEE_TOO_LARGE -8
   /* The deadline passed before an outcome. */
#define AIMEE_TIMEOUT -9
   /* Stored state failed its own check. */
#define AIMEE_INTEGRITY -10
   /* The caller withdrew before an outcome. */
#define AIMEE_CANCELLED -11

   /* --- domain bands, one hundred per domain --------------------------- */

   /* A domain code is used only where no universal code says it.
    * DB2_SPEND_ERR_DENIED is AIMEE_DENIED and needs no band code;
    * DB2_ERR_TENANT_BEGIN is not any universal failure -- the transaction
    * opened and the GUCs did not take -- so it keeps one.
    *
    * Successes count up from the band, failures down from its negation.
    *
    * The bands are named for what the code does, not for which store or which
    * binary it lives in. Storage is one Postgres, whoever hosts it, and the
    * band does not move or change name when the schema owner does. */
#define AIMEE_BAND_TENANCY   100 /* tenancy and identity */
#define AIMEE_BAND_STORAGE   200 /* postgres, pools, transactions */
#define AIMEE_BAND_ORG       300 /* budget, egress, rate, spend, telemetry */
#define AIMEE_BAND_CUSTODY   400 /* vault, enrolments, witness */
#define AIMEE_BAND_CODEINDEX 500 /* code index and projects */
#define AIMEE_BAND_KBDOC     600 /* knowledge base documents and ingest */
#define AIMEE_BAND_MEMORY    700 /* memory and facts */
#define AIMEE_BAND_CSS       800 /* css analysis and rendering */
#define AIMEE_BAND_TRANSPORT 900 /* http client, sockets */

   /* 1 if `r` says the work finished and succeeded. */
#define AIMEE_SUCCEEDED(r) ((r) > 0)
   /* 1 if `r` says the work finished and failed. */
#define AIMEE_FAILED_P(r) ((r) < 0)
   /* 1 if `r` says the work has not finished. Not an outcome either way. */
#define AIMEE_PENDING_P(r) ((r) == 0)

#ifdef __cplusplus
}
#endif

#endif /* DEC_AIMEE_RESULT_H */
