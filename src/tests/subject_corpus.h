/* subject_corpus.h — the ONE definition of what a data-plane subject is.
 *
 * A subject (the identity a `/v1` write grant is keyed by) has its grammar
 * encoded in THREE independent places, because the layers cannot share code:
 *
 *   1. the `subject` CHECK on kb_write_tier_grant and kb_management_identity_intent
 *      (db2/schema.sql) — a Postgres regex
 *   2. db2_intent_canonical_actor (db2/management_intent_fields.h) — kb's C side
 *   3. server_identity_subject_valid (shared/auth_token_verify.c) — the server's,
 *      which links neither DB2_OBJS nor libpq (see scripts/check_tier_deps.sh),
 *      so it cannot include (2)
 *
 * Three copies of one rule drift, and the drift is SILENT AND ASYMMETRIC:
 * whichever copy is stricter wins, so a subject the database accepts but a
 * verifier rejects mints a token that is then refused as malformed, while a
 * subject a verifier accepts but the database rejects fails at grant time. Both
 * have already happened on this branch — once when the CHECK was widened without
 * the server (5f486574), and once when the fix widened the management actor as a
 * side effect.
 *
 * So the corpus lives here, and every copy is tested against it rather than
 * against its own author's assumptions. Adding a form means adding it here first;
 * the tests then tell you which copies do not implement it yet.
 *
 * The Postgres regex is checked against this same corpus by
 * scripts/per-user-identity-authority-pg17-test.sql, generated from this file by
 * scripts/gen-subject-corpus-sql.py so the two cannot fall out of step.
 */
#ifndef AIMEE_TESTS_SUBJECT_CORPUS_H
#define AIMEE_TESTS_SUBJECT_CORPUS_H

typedef struct
{
   const char *subject;
   int accept;      /* 1 = every copy must accept, 0 = every copy must reject */
   const char *why; /* what this case is for; shown on failure */
} subject_case_t;

static const subject_case_t SUBJECT_CORPUS[] = {
    /* ---- owner: the single-org bearer principal ---------------------------- */
    {"owner", 1, "the bearer principal"},

    /* ---- bare host account: the PAM login's form --------------------------- */
    {"alice", 1, "a plain host account"},
    {"a", 1, "a one-character account name"},
    {"svc_user-1.2", 1, "underscores, dashes and dots"},
    {"_systemd", 1, "a leading underscore is legal"},
    {"0day", 1, "a leading digit is legal in practice"},
    {"abcdefghijklmnopqrstuvwxyz123456", 1, "32 characters, the Linux limit"},
    {"abcdefghijklmnopqrstuvwxyz1234567", 0, "33 characters, over the limit"},
    {"-alice", 0, "a leading dash could be read as an option by a helper"},
    {".alice", 0, "a leading dot is not a login name"},
    {"al ice", 0, "a space is not a login name"},
    {"al/ice", 0, "a slash is a path separator"},
    {"al\tice", 0, "a tab is a control character"},
    {"", 0, "empty"},

    /* ---- oidc: issuer-scoped, because a sub is unique only within an issuer -- */
    {"oidc:https%3A//idp.example:alice", 1, "the issuer's ':' is percent-encoded"},
    {"oidc:issuer:subject", 1, "a minimal issuer/subject pair"},
    {"oidc:a%3Ab:c%25d", 1, "both components percent-encoded"},
    {"oidc:issuer:", 0, "an empty subject component"},
    {"oidc::subject", 0, "an empty issuer component"},
    {"oidc:issuer:sub:extra", 0, "a fourth component is ambiguous"},
    {"oidc:bad%3aissuer:subject", 0, "lowercase %3a is not the canonical encoding"},
    {"oidc:bad%20issuer:subject", 0, "%20 is not an allowed encoding here"},
    {"oidc:onlytwoparts", 0, "prefixed but missing a component"},

    /* ---- cert: issuer-scoped, serial normalized to lowercase hex ------------ */
    {"cert:CN=aimee-ca:a1b", 1, "a normalized serial"},
    {"cert:issuer:01af", 1, "lowercase hex"},
    {"cert:issuer:01AF", 0, "uppercase hex is not the normalized form"},
    {"cert:issuer:nothex", 0, "a serial must be hex"},
    {"cert::01af", 0, "an empty issuer component"},

    /* ---- things that are no form at all ------------------------------------ */
    {"not an identity key", 0, "spaces: matches nothing"},
    {"webuser:alice", 0, "a vault-principal namespace is not an identity key"},
    {"uid:1000", 0, "likewise a transport attestation"},
    {"pam:alice", 0, "the PAM form is bare; a prefix is not a second spelling"},
};

#define SUBJECT_CORPUS_N (sizeof(SUBJECT_CORPUS) / sizeof(SUBJECT_CORPUS[0]))

#endif /* AIMEE_TESTS_SUBJECT_CORPUS_H */
