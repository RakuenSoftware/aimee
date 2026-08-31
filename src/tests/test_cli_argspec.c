/* test_cli_argspec.c: a served argument spec must build the SAME body the
 * compiled marshaller does.
 *
 * This is the only property that makes moving argument handling server-side
 * safe. The manifest can already add a command, route it and document it; the
 * spec lets it describe the command's arguments too. But every one of these
 * methods works today through a hand-written marshaller, and a spec that is
 * merely plausible would change what the CLI sends for commands people already
 * run — silently, because both bodies are valid JSON and the server answers
 * either.
 *
 * So the test is differential, not descriptive: for each specced method it runs
 * the real marshaller and the interpreter over the same argv and compares the
 * rendered JSON. A spec is allowed to ship only once it is indistinguishable.
 *
 * The samples per method are deliberately awkward — flag absent, flag present,
 * positional instead of flag, empty string, unknown extra flag — because the
 * cases that differ are the edges, and a spec that only agrees on the happy
 * path is a spec that breaks the first operator who omits an argument.
 */
#include "cli_argspec.h"
#include "cli_v1_routes_internal.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

static void fail(const char *what, const char *detail)
{
   g_fail++;
   printf("FAIL %s: %s\n", what, detail ? detail : "");
}

/* ---- the SHIPPED specs, and the samples that prove them ----------------- */

/* The same rows the server serves. Included, not copied: a test with its own
 * specs would prove specs that are not the ones shipped. */
typedef struct
{
   const char *method;
   const char *spec;
} argspec_row_t;

static const argspec_row_t SHIPPED[] = {
#include "../server/cli_argspec_defs_data.h"
};

/* Samples are deliberately awkward — flag absent, flag present, positional
 * instead of flag, empty string, unknown extra flag — because the cases that
 * differ are the edges. A spec that only agrees on the happy path is a spec
 * that breaks the first operator who omits an argument. */
typedef struct
{
   const char *method;
   const char *argv[8];
} sample_t;

/* A key long enough to breach the user_capture family's 512-char limit once
   the prefix is added. The limit is the rule a spec-derived sample set can
   never reach on its own: every key it invents is short, so the limit is
   invisible and a spec that omitted it would pass. */
#define LONG_KEY                                                                                   \
   "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"  \
   "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"  \
   "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"  \
   "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"  \
   "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"  \
   "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"

static const sample_t SAMPLES[] = {
    {"kb.erase-subject", {NULL}},
    {"kb.erase-subject", {"subject@example.test", NULL}},
    {"kb.erase-subject", {"", NULL}},
    {"kb.erase-subject",
     {"subject@example.test", "--request-id", "erase-request-0123456789", NULL}},
    {"kb.erase-subject", {"--request-id=erase-request-0123456789", "subject@example.test", NULL}},
    {"kb.erase-subject", {"subject@example.test", "--request-id", "", NULL}},
    {"kb.erase-subject", {"subject@example.test", "extra", NULL}},

    /* memory.delete / memory.get / memory.embed — the numeric width samples.
       memory.delete SHIPPED with a spec saying atoi() where the marshaller uses
       atoll(); every id in the suite was small, so nothing disagreed. These
       straddle 2^31 and 2^63 deliberately, and a fractional value separates
       atof() from both. Without them the three lenient parses are
       indistinguishable and the spec can name the wrong one freely. */
    {"memory.delete", {"2147483647", NULL}},
    {"memory.delete", {"2147483648", NULL}},
    {"memory.delete", {"4294967296", NULL}},
    {"memory.delete", {"9007199254740993", NULL}},
    {"memory.delete", {"-2147483649", NULL}},
    {"memory.delete", {"12.9", NULL}},

    {"memory.get", {NULL}},
    {"memory.get", {"42", NULL}},
    {"memory.get", {"2147483648", NULL}},
    {"memory.get", {"9007199254740993", NULL}},
    {"memory.get", {"12x", NULL}},
    {"memory.get", {"abc", NULL}},
    {"memory.get", {"", NULL}},
    {"memory.get", {"7", "--as-of", "2026-01-01", NULL}},
    {"memory.get", {"7", "--as_of", "2026-01-01", NULL}},
    {"memory.get", {"7", "--as-of", "", NULL}},
    {"memory.get", {"7", "--as-of", "a", "--as_of", "b", NULL}},

    {"memory.embed", {NULL}},
    {"memory.embed", {"--all", NULL}},
    {"memory.embed", {"5", NULL}},
    {"memory.embed", {"12.9", NULL}},
    {"memory.embed", {"2147483648", NULL}},
    {"memory.embed", {"abc", NULL}},
    {"memory.embed", {"", NULL}},
    {"memory.embed", {"5", "--version", "v2", NULL}},
    {"memory.embed", {"5", "--version", "", NULL}},
    {"memory.embed", {"--all", "5", "--version", "v2", NULL}},

    /* The cwd family. cwd is the same value on both sides here -- the test runs
       the compiled marshaller and the interpreter in one process -- so these
       samples prove the OTHER fields and prove the field is emitted at all. A
       spec that forgot cwd entirely fails, which is the assertion that matters:
       a served spec silently dropping cwd would send a project-scoped query
       with no project. */
    {"index.find", {NULL}},
    {"index.find", {"foo", NULL}},
    {"index.find", {"foo", "--scope", "all", NULL}},
    {"index.find", {"--scope", "all", NULL}},
    {"index.find", {"", NULL}},
    {"index.find", {"foo", "--scope", "", NULL}},
    {"index.find", {"--json", "foo", NULL}},
    {"index.find", {"foo", "--project", "explicit-project", NULL}},

    {"memory.list", {NULL}},
    {"memory.list", {"--tier", "L2", NULL}},
    {"memory.list", {"--kind", "fact", "--limit", "5", NULL}},
    {"memory.list", {"--limit", "abc", NULL}},
    {"memory.list", {"--limit", "", NULL}},
    {"memory.list", {"--project", "p", "--workspace", "w", "--scope", "s", NULL}},
    {"memory.list", {"--tier", "", NULL}},
    {"memory.list", {"stray", NULL}},

    /* Generated candidates: every flag alone, all together, an empty value,
       the positionals, and one argument too many. Spec-derived, so blind to any
       rule the spec omits -- which is why the adversarial and both-sources
       samples exist. These catch gross errors; they do not certify a spec. */

    {"identity.snapshot", {NULL}},
    {"identity.snapshot", {"--out", "v", NULL}},
    {"identity.snapshot", {"--out", "v", NULL}},
    {"identity.snapshot", {"--out", "", NULL}},
    {"identity.snapshot", {"--unknown-flag", "x", NULL}},
    {"index.find_callers", {NULL}},
    {"index.find_callers", {"--scope", "v", NULL}},
    {"index.find_callers", {"--scope", "v", NULL}},
    {"index.find_callers", {"--scope", "", NULL}},
    {"index.find_callers", {"p0", "p1", NULL}},
    {"index.find_callers", {"", "", NULL}},
    {"index.find_callers", {"p0", "p1", "p2", NULL}},
    {"index.find_callers", {"--unknown-flag", "x", NULL}},
    {"index.find_callers", {"p0", "--project", "explicit-project", NULL}},
    {"kb.search", {NULL}},
    {"kb.search", {"--project", "v", NULL}},
    {"kb.search", {"--scope", "v", NULL}},
    {"kb.search", {"--max", "v", NULL}},
    {"kb.search", {"--fusion-mode", "v", NULL}},
    {"kb.search", {"--embed", "v", NULL}},
    {"kb.search", {"--project", "v", "--scope", "v", "--max", "v", NULL}},
    {"kb.search", {"--project", "", NULL}},
    {"kb.search", {"p0", NULL}},
    {"kb.search", {"", NULL}},
    {"kb.search", {"p0", "p1", NULL}},
    {"kb.search", {"--unknown-flag", "x", NULL}},
    {"wm.get", {NULL}},
    {"wm.get", {"p0", NULL}},
    {"wm.get", {"", NULL}},
    {"wm.get", {"p0", "p1", NULL}},
    {"wm.get", {"--unknown-flag", "x", NULL}},
    {"wm.list", {NULL}},
    {"wm.list", {"--category", "v", NULL}},
    {"wm.list", {"--category", "v", NULL}},
    {"wm.list", {"--category", "", NULL}},
    {"wm.list", {"--unknown-flag", "x", NULL}},
    {"wm.set", {NULL}},
    {"wm.set", {"--category", "v", NULL}},
    {"wm.set", {"--ttl", "v", NULL}},
    {"wm.set", {"--category", "v", "--ttl", "v", NULL}},
    {"wm.set", {"--category", "", NULL}},
    {"wm.set", {"p0", "p1", NULL}},
    {"wm.set", {"", "", NULL}},
    {"wm.set", {"p0", "p1", "p2", NULL}},
    {"wm.set", {"--unknown-flag", "x", NULL}},

    /* memory.recall -- the cascade, probed at every step and in the order that
       distinguishes them. --task AND a positional together is the sample that
       separates "flag first" from "positional first"; an empty --task must fall
       through rather than win; nothing at all must yield the literal. */
    {"memory.recall", {NULL}},
    {"memory.recall", {"--task", "t", NULL}},
    {"memory.recall", {"p0", NULL}},
    {"memory.recall", {"--task", "t", "p0", NULL}},
    {"memory.recall", {"--query", "q", NULL}},
    {"memory.recall", {"--task", "t", "--query", "q", NULL}},
    {"memory.recall", {"p0", "--query", "q", NULL}},
    {"memory.recall", {"--task", "", "--query", "q", NULL}},
    {"memory.recall", {"--task", "", NULL}},
    {"memory.recall", {"", NULL}},
    {"memory.recall", {"--session-start", NULL}},
    {"memory.recall", {"--limit-tokens", "500", NULL}},
    {"memory.recall", {"--limit-tokens", "0", NULL}},
    {"memory.recall", {"--limit-tokens", "-5", NULL}},
    {"memory.recall", {"--project", "p", "--scope", "s", NULL}},

    /* The session cascade. --session must beat $AIMEE_SESSION_ID, which must
       beat the literal; with the variable set by main() these three samples are
       what tells the orders apart. */
    {"wm.list", {"--session", "flag-session", NULL}},
    {"wm.list", {"--session", "", NULL}},
    {"wm.get", {"k", "--session", "flag-session", NULL}},
    {"wm.set", {"k", "v", "--session", "flag-session", NULL}},

    /* session.attach / detach -- the cascade plus a literal default, and the
       --subscribe threshold where ZERO is a real value. */
    {"session.attach", {NULL}},
    {"session.attach", {"sid", NULL}},
    {"session.attach", {"", NULL}},
    {"session.attach", {"--session", "flag-session", NULL}},
    {"session.attach", {"sid", "--surface", "web", NULL}},
    {"session.attach", {"sid", "--surface", "", NULL}},
    {"session.attach", {"sid", "--subscribe", "0", NULL}},
    {"session.attach", {"sid", "--subscribe", "-1", NULL}},
    {"session.attach", {"sid", "--subscribe", "7", NULL}},
    {"session.attach", {"sid", "--persistent", NULL}},
    {"session.attach", {"sid", "--target", "t", "--owner", "o", NULL}},
    {"session.attach", {"sid", "--target", "", NULL}},

    {"session.detach", {NULL}},
    {"session.detach", {"sid", NULL}},
    {"session.detach", {"", NULL}},
    {"session.detach", {"sid", "--attach-id", "a1", NULL}},
    {"session.detach", {"sid", "--attach-id", "", NULL}},
    {"session.detach", {"--session", "flag-session", NULL}},

    /* Generated candidates: every flag alone, three together, an empty value,
       a trailing-garbage number, the positionals, and one argument too many.
       Spec-derived, so blind to any rule the spec omits. */

    {"eval.run", {NULL}},
    {"eval.run", {"--ablation", "v", NULL}},
    {"eval.run", {"--runs", "v", NULL}},
    {"eval.run", {"--seed", "v", NULL}},
    {"eval.run", {"--ablation", "v", "--runs", "v", "--seed", "v", NULL}},
    {"eval.run", {"--ablation", "", NULL}},
    {"eval.run", {"--ablation", "12x", NULL}},
    {"eval.run", {"p0", NULL}},
    {"eval.run", {"", NULL}},
    {"eval.run", {"p0", "p1", NULL}},
    {"eval.run", {"--unknown-flag", "x", NULL}},
    {"identity.diff", {NULL}},
    {"identity.diff", {"--flip-threshold", "v", NULL}},
    {"identity.diff", {"--flip-threshold", "v", NULL}},
    {"identity.diff", {"--flip-threshold", "", NULL}},
    {"identity.diff", {"--flip-threshold", "12x", NULL}},
    {"identity.diff", {"p0", "p1", NULL}},
    {"identity.diff", {"", "", NULL}},
    {"identity.diff", {"p0", "p1", "p2", NULL}},
    {"identity.diff", {"--unknown-flag", "x", NULL}},

    {"memory.benchmark", {NULL}},
    {"memory.benchmark", {"suite-x", NULL}},
    {"memory.benchmark", {"", NULL}},
    {"memory.benchmark", {"a", "b", NULL}},

    /* Generated candidates: every flag alone, three together, an empty value,
       a trailing-garbage number, the positionals, and one argument too many.
       Spec-derived, so blind to any rule the spec omits. */

    {"memory.search", {NULL}},
    {"memory.search", {"--limit", "v", NULL}},
    {"memory.search", {"--limit", "v", NULL}},
    {"memory.search", {"--limit", "", NULL}},
    {"memory.search", {"--limit", "12x", NULL}},
    {"memory.search", {"--unknown-flag", "x", NULL}},
    {"worktree.gc", {NULL}},
    {"worktree.gc", {"--days", "v", NULL}},
    {"worktree.gc", {"--force", "v", NULL}},
    {"worktree.gc", {"--dry-run", "v", NULL}},
    {"worktree.gc", {"--days", "v", "--force", "v", "--dry-run", "v", NULL}},
    {"worktree.gc", {"--days", "", NULL}},
    {"worktree.gc", {"--days", "12x", NULL}},
    {"worktree.gc", {"--unknown-flag", "x", NULL}},

    /* Generated candidates: every flag alone, three together, an empty value,
       a trailing-garbage number, the positionals, and one argument too many.
       Spec-derived, so blind to any rule the spec omits. */

    {"pipeline.start", {NULL}},
    {"pipeline.start", {"--done-bar", "v", NULL}},
    {"pipeline.start", {"--base-branch", "v", NULL}},
    {"pipeline.start", {"--repo-root", "v", NULL}},
    {"pipeline.start", {"--brief", "v", NULL}},
    {"pipeline.start", {"--head-branch", "v", NULL}},
    {"pipeline.start", {"--remote", "v", NULL}},
    {"pipeline.start", {"--worktree-path", "v", NULL}},
    {"pipeline.start", {"--done-bar", "v", "--base-branch", "v", "--repo-root", "v", NULL}},
    {"pipeline.start", {"--done-bar", "", NULL}},
    {"pipeline.start", {"--done-bar", "12x", NULL}},
    {"pipeline.start", {"p0", NULL}},
    {"pipeline.start", {"", NULL}},
    {"pipeline.start", {"p0", "p1", NULL}},
    {"pipeline.start", {"--unknown-flag", "x", NULL}},
    {"skill.list", {NULL}},
    {"skill.list", {"--unknown-flag", "x", NULL}},

    /* Generated candidates: every flag alone, three together, an empty value,
       a trailing-garbage number, the positionals, and one argument too many.
       Spec-derived, so blind to any rule the spec omits. */

    {"skill.archive", {NULL}},
    {"skill.archive", {"--unknown-flag", "x", NULL}},
    {"skill.eval", {NULL}},
    {"skill.eval", {"--unknown-flag", "x", NULL}},
    {"skill.show", {NULL}},
    {"skill.show", {"--unknown-flag", "x", NULL}},

    /* ARITY samples: every positional count from one to n+1, for every spec.
       index.structure shipped wrong because its samples supplied two
       positionals and three but never ONE, and one positional is how the
       command is actually used. Generated from the spec, so still blind to what
       the spec omits -- but no longer blind to the counts it implies. */

    {"index.find", {"p0", NULL}},
    {"index.find", {"p0", "p1", NULL}},
    {"index.find_callers", {"p0", NULL}},
    {"wm.set", {"p0", NULL}},
    {"identity.diff", {"p0", NULL}},
    {"provider.models", {"p0", NULL}},
    {"provider.models", {"p0", "p1", NULL}},
    {"model.episodes", {"p0", NULL}},
    {"model.episodes", {"p0", "p1", NULL}},
    {"graph.sync_code", {"p0", NULL}},
    {"graph.sync_code", {"p0", "p1", NULL}},
    {"eval.results", {"p0", NULL}},
    {"eval.results", {"p0", "p1", NULL}},
    {"cert.revoke", {"p0", NULL}},
    {"cert.revoke", {"p0", "p1", NULL}},
    {"vault.capability", {"p0", NULL}},
    {"vault.capability", {"p0", "p1", NULL}},
    {"vault.capability", {"p0", "p1", "p2", NULL}},
    {"vault.delete", {"p0", NULL}},
    {"vault.delete", {"p0", "p1", NULL}},
    {"vault.delete", {"p0", "p1", "p2", NULL}},
    {"vault.set", {"p0", NULL}},
    {"vault.set", {"p0", "p1", NULL}},
    {"vault.set", {"p0", "p1", "p2", NULL}},
    {"vault.set", {"p0", "p1", "p2", "p3", NULL}},
    {"vault.set_server", {"p0", NULL}},
    {"vault.set_server", {"p0", "p1", NULL}},
    {"vault.set_server", {"p0", "p1", "p2", NULL}},
    {"vault.set_server", {"p0", "p1", "p2", "p3", NULL}},
    {"graph.explain", {"p0", NULL}},
    {"graph.explain", {"p0", "p1", NULL}},
    {"aux.test", {"p0", NULL}},
    {"aux.test", {"p0", "p1", NULL}},
    {"aux.test", {"p0", "p1", "p2", NULL}},
    {"aux.test", {"p0", "p1", "p2", "p3", NULL}},
    {"config.get", {"p0", NULL}},
    {"config.get", {"p0", "p1", NULL}},
    {"config.set", {"p0", NULL}},
    {"config.set", {"p0", "p1", NULL}},
    {"config.set", {"p0", "p1", "p2", NULL}},
    {"delegate.aggregate", {"p0", NULL}},
    {"delegate.aggregate", {"p0", "p1", NULL}},
    {"evidence.fidelity_retrieval_event", {"p0", NULL}},
    {"evidence.fidelity_retrieval_event", {"p0", "p1", NULL}},
    {"evidence.provenance_retrieval_event", {"p0", NULL}},
    {"evidence.provenance_retrieval_event", {"p0", "p1", NULL}},
    {"evidence.trace_retrieval_event", {"p0", NULL}},
    {"evidence.trace_retrieval_event", {"p0", "p1", NULL}},
    {"index.scan", {"p0", NULL}},
    {"index.scan", {"p0", "p1", NULL}},
    {"index.scan", {"p0", "p1", "p2", NULL}},
    {"kb.ingest", {"p0", NULL}},
    {"kb.ingest", {"p0", "p1", NULL}},
    {"kb.status", {"p0", NULL}},
    {"kb.status", {"p0", "p1", NULL}},
    {"kb.update", {"p0", NULL}},
    {"kb.update", {"p0", "p1", NULL}},
    {"kb.update", {"p0", "p1", "p2", NULL}},
    {"curator.implements", {"p0", NULL}},
    {"curator.implements", {"p0", "p1", NULL}},
    {"curator.synthesize", {"p0", NULL}},
    {"curator.synthesize", {"p0", "p1", NULL}},
    {"provider.quota", {"p0", NULL}},
    {"provider.quota", {"p0", "p1", NULL}},
    {"provider.show", {"p0", NULL}},
    {"provider.show", {"p0", "p1", NULL}},
    {"provider.test", {"p0", NULL}},
    {"provider.test", {"p0", "p1", NULL}},
    {"model.episodes", {"p0", NULL}},
    {"model.episodes", {"p0", "p1", NULL}},
    {"cert.issue", {"p0", NULL}},
    {"cert.issue", {"p0", "p1", NULL}},
    {"job.cancel", {"p0", NULL}},
    {"job.cancel", {"p0", "p1", NULL}},
    {"job.status", {"p0", NULL}},
    {"job.status", {"p0", "p1", NULL}},
    {"jobs.cancel", {"p0", NULL}},
    {"jobs.cancel", {"p0", "p1", NULL}},
    {"jobs.logs", {"p0", NULL}},
    {"jobs.logs", {"p0", "p1", NULL}},
    {"jobs.status", {"p0", NULL}},
    {"jobs.status", {"p0", "p1", NULL}},
    {"rules.delete", {"p0", NULL}},
    {"rules.delete", {"p0", "p1", NULL}},
    {"job.start", {"p0", NULL}},
    {"job.start", {"p0", "p1", NULL}},
    {"notes.search", {"p0", NULL}},
    {"notes.search", {"p0", "p1", NULL}},
    {"pipeline.advance", {"p0", NULL}},
    {"pipeline.advance", {"p0", "p1", NULL}},
    {"pipeline.cancel", {"p0", NULL}},
    {"pipeline.cancel", {"p0", "p1", NULL}},
    {"pipeline.gate", {"p0", NULL}},
    {"pipeline.gate", {"p0", "p1", NULL}},
    {"pipeline.gate", {"p0", "p1", "p2", NULL}},
    {"pipeline.resume", {"p0", NULL}},
    {"pipeline.resume", {"p0", "p1", NULL}},
    {"pipeline.show", {"p0", NULL}},
    {"pipeline.show", {"p0", "p1", NULL}},
    {"pipeline.status", {"p0", NULL}},
    {"pipeline.status", {"p0", "p1", NULL}},
    {"session.brief", {"p0", NULL}},
    {"session.brief", {"p0", "p1", NULL}},
    {"index.deps", {"p0", NULL}},
    {"index.deps", {"p0", "p1", NULL}},
    {"index.deps", {"--project", "explicit-project", "--scope", "all", NULL}},
    {"session.close", {"p0", NULL}},
    {"session.close", {"p0", "p1", NULL}},
    {"session.get", {"p0", NULL}},
    {"session.get", {"p0", "p1", NULL}},

    /* Generated candidates, sampled at every arity the spec implies. */

    {"index.blast_radius", {NULL}},
    {"index.blast_radius", {"p0", NULL}},
    {"index.blast_radius", {"p0", "p1", NULL}},
    {"index.blast_radius", {"p0", "p1", "p2", NULL}},
    {"index.blast_radius", {"", NULL}},
    {"index.blast_radius", {"", "", NULL}},
    {"index.blast_radius", {"--unknown-flag", "x", NULL}},
    {"index.blast_radius", {"p0", "--project", "explicit-project", "--scope", "all", NULL}},
    {"index.hybrid", {NULL}},
    {"index.hybrid", {"--scope", "v", NULL}},
    {"index.hybrid", {"--scope", "v", NULL}},
    {"index.hybrid", {"--scope", "", NULL}},
    {"index.hybrid", {"--scope", "12x", NULL}},
    {"index.hybrid", {"p0", NULL}},
    {"index.hybrid", {"p0", "p1", NULL}},
    {"index.hybrid", {"", NULL}},
    {"index.hybrid", {"--unknown-flag", "x", NULL}},
    {"index.hybrid", {"p0", "--project", "explicit-project", NULL}},
    {"index.structure", {NULL}},
    {"index.structure", {"p0", NULL}},
    {"index.structure", {"p0", "p1", NULL}},
    {"index.structure", {"p0", "p1", "p2", NULL}},
    {"index.structure", {"", NULL}},
    {"index.structure", {"", "", NULL}},
    {"index.structure", {"--unknown-flag", "x", NULL}},
    {"index.structure", {"p0", "--project", "explicit-project", "--scope", "all", NULL}},
    {"skill.pin", {NULL}},
    {"skill.pin", {"--unknown-flag", "x", NULL}},
    {"skill.unpin", {NULL}},
    {"skill.unpin", {"--unknown-flag", "x", NULL}},

    /* Generated candidates, sampled at every arity the spec implies. */

    {"skill.lint", {NULL}},
    {"skill.lint", {"--unknown-flag", "x", NULL}},
    {"skill.patch", {NULL}},
    {"skill.patch", {"--unknown-flag", "x", NULL}},

    /* init.run discards its arguments, so every shape must build the same body. */
    {"init.run", {NULL}},
    {"init.run", {"anything", NULL}},
    {"init.run", {"--flag", "v", NULL}},
    {"init.run", {"", NULL}},
    {"init.run", {"a", "b", "c", NULL}},

    /* The `--flag=value` form. cli_args_parse takes it as readily as
       `--flag value`, and nothing here exercised it, so a spec that named a
       flag its marshaller hand-scans for would agree on one form and diverge on
       the other. */

    {"index.find", {"--scope=v", NULL}},
    {"memory.list", {"--tier=v", NULL}},
    {"memory.list", {"--kind=v", NULL}},
    {"identity.snapshot", {"--out=v", NULL}},
    {"index.find_callers", {"--scope=v", NULL}},
    {"kb.search", {"--project=v", NULL}},
    {"kb.search", {"--scope=v", NULL}},
    {"wm.list", {"--category=v", NULL}},
    {"wm.set", {"--category=v", NULL}},
    {"wm.set", {"--ttl=v", NULL}},
    {"eval.run", {"--ablation=v", NULL}},
    {"eval.run", {"--runs=v", NULL}},
    {"identity.diff", {"--flip-threshold=v", NULL}},
    {"memory.search", {"--limit=v", NULL}},
    {"memory.search", {"--project=v", NULL}},
    {"worktree.gc", {"--days=v", NULL}},
    {"worktree.gc", {"--force=v", NULL}},
    {"index.hybrid", {"--scope=v", NULL}},
    {"session.presence", {"--owner=v", NULL}},
    {"insights.overview", {"--days=v", NULL}},
    {"provider.list", {"--available=v", NULL}},
    {"provider.list", {"--all=v", NULL}},
    {"provider.models", {"--json=v", NULL}},
    {"catalog.list", {"--capability=v", NULL}},
    {"catalog.list", {"--json=v", NULL}},
    {"trigger.list", {"--status=v", NULL}},
    {"model.episodes", {"--agent=v", NULL}},
    {"dogfood.report", {"--month=v", NULL}},
    {"dogfood.report", {"--dir=v", NULL}},
    {"graph.explain", {"--limit=v", NULL}},
    {"dogfood.review", {"--month=v", NULL}},
    {"dogfood.review", {"--dir=v", NULL}},
    {"index.scan", {"--force=v", NULL}},
    {"kb.ingest", {"--force=v", NULL}},
    {"kb.ingest", {"--embed=v", NULL}},
    {"kb.update", {"--embed=v", NULL}},
    {"model.episodes", {"--agent=v", NULL}},
    {"cert.issue", {"--days=v", NULL}},
    {"job.cancel", {"--job-id=v", NULL}},
    {"job.cancel", {"--reason=v", NULL}},
    {"job.status", {"--job-id=v", NULL}},
    {"job.status", {"--reason=v", NULL}},
    {"jobs.cancel", {"--job-id=v", NULL}},
    {"jobs.cancel", {"--reason=v", NULL}},
    {"jobs.logs", {"--job-id=v", NULL}},
    {"jobs.logs", {"--reason=v", NULL}},
    {"jobs.status", {"--job-id=v", NULL}},
    {"jobs.status", {"--reason=v", NULL}},
    {"kb.reembed", {"--confirm=v", NULL}},
    {"kb.reembed", {"--force=v", NULL}},
    {"rules.delete", {"--id=v", NULL}},
    {"job.start", {"--plan-id=v", NULL}},
    {"job.start", {"--parallel=v", NULL}},
    {"session.list", {"--limit=v", NULL}},
    {"curator.contradictions", {"--limit=v", NULL}},
    {"job.list", {"--limit=v", NULL}},
    {"jobs.list", {"--limit=v", NULL}},
    {"notes.search", {"--query=v", NULL}},
    {"notes.search", {"--limit=v", NULL}},
    {"pipeline.advance", {"--artifact=v", NULL}},
    {"pipeline.advance", {"--artifact-hash=v", NULL}},
    {"pipeline.cancel", {"--artifact=v", NULL}},
    {"pipeline.cancel", {"--artifact-hash=v", NULL}},
    {"pipeline.gate", {"--reason=v", NULL}},
    {"pipeline.gate", {"--operator-principal=v", NULL}},
    {"pipeline.list", {"--state=v", NULL}},
    {"pipeline.resume", {"--artifact=v", NULL}},
    {"pipeline.resume", {"--artifact-hash=v", NULL}},
    {"pipeline.show", {"--artifact=v", NULL}},
    {"pipeline.show", {"--artifact-hash=v", NULL}},
    {"pipeline.status", {"--artifact=v", NULL}},
    {"pipeline.status", {"--artifact-hash=v", NULL}},
    {"api.enable", {"--vscode=v", NULL}},
    {"api.enable", {"--port=v", NULL}},
    {"session.brief", {"--session=v", NULL}},
    {"session.brief", {"--list=v", NULL}},
    {"index.deps", {"--tier=v", NULL}},
    {"index.deps", {"--review=v", NULL}},
    {"session.close", {"--session=v", NULL}},
    {"session.get", {"--session=v", NULL}},

    /* The user_capture family: the join, the constant prefix, the length
       refusal, and the --content fallback. The oversized key is the sample the
       whole 512-limit exists for, and the one a spec-derived generator would
       never produce. */

    {"memory.identity", {NULL}},
    {"memory.identity", {"k", NULL}},
    {"memory.identity", {"k", "one", NULL}},
    {"memory.identity", {"k", "one", "two", "three", NULL}},
    {"memory.identity", {"k", "--content", "body", NULL}},
    {"memory.identity", {"--content", "body", NULL}},
    {"memory.identity", {"", "body", NULL}},
    {"memory.identity", {"k", "", NULL}},
    {"memory.identity", {LONG_KEY, "body", NULL}},
    {"memory.prefer", {NULL}},
    {"memory.prefer", {"k", NULL}},
    {"memory.prefer", {"k", "one", NULL}},
    {"memory.prefer", {"k", "one", "two", "three", NULL}},
    {"memory.prefer", {"k", "--content", "body", NULL}},
    {"memory.prefer", {"--content", "body", NULL}},
    {"memory.prefer", {"", "body", NULL}},
    {"memory.prefer", {"k", "", NULL}},
    {"memory.prefer", {LONG_KEY, "body", NULL}},
    {"memory.archive", {NULL}},
    {"memory.archive", {"k", NULL}},
    {"memory.archive", {"k", "one", NULL}},
    {"memory.archive", {"k", "one", "two", "three", NULL}},
    {"memory.archive", {"k", "--content", "body", NULL}},
    {"memory.archive", {"--content", "body", NULL}},
    {"memory.archive", {"", "body", NULL}},
    {"memory.archive", {"k", "", NULL}},
    {"memory.archive", {LONG_KEY, "body", NULL}},

    {"memory.store", {NULL}},
    {"memory.store", {"k", NULL}},
    {"memory.store", {"k", "one", "two", NULL}},
    {"memory.store", {"--key", "k", "--content", "c", NULL}},
    {"memory.store", {"k", "--content", "c", NULL}},
    {"memory.store", {"k", "v", "--tier", "L2", "--kind", "fact", NULL}},
    {"memory.store", {"k", "v", "--confidence", "0.5", NULL}},
    {"memory.store", {"k", "v", "--confidence", "abc", NULL}},
    {"memory.store", {"", "v", NULL}},
    {"memory.store", {"k", "", NULL}},
    {"memory.store", {"k", "v", "--session", "s", NULL}},

    /* delegate.log -- an ARITY rule, so the samples that matter are the ones
       that must be REFUSED. The test treats both sides refusing as agreement
       and flags a one-sided refusal, which is exactly the assertion wanted
       here. `--json x` must be ACCEPTED: the marshaller passes NULL bool flags,
       so x is the flag's value and never becomes a positional. */
    {"delegate.log", {NULL}},
    {"delegate.log", {"--json", NULL}},
    {"delegate.log", {"--json", "x", NULL}},
    {"delegate.log", {"42", NULL}},
    {"delegate.log", {"a", "b", NULL}},
    {"delegate.log", {"", NULL}},
    {"delegate.log", {"--", NULL}},
    {"delegate.log", {"-x", NULL}},

    /* session.presence — one optional flag, empty dropped. */
    {"session.presence", {NULL}},
    {"session.presence", {"--owner", "ada", NULL}},
    {"session.presence", {"--owner", "", NULL}},
    {"session.presence", {"stray", NULL}},

    /* insights.overview — the clamp, probed on BOTH sides and at each edge.
       These are the samples that would catch a spec that named the field and
       forgot its range: 0 and 9999 render differently under a clamp than
       without one, while 30 and 200 render identically either way. */
    {"insights.overview", {NULL}},
    {"insights.overview", {"--days", "200", NULL}},
    {"insights.overview", {"--days", "0", NULL}},
    {"insights.overview", {"--days", "-5", NULL}},
    {"insights.overview", {"--days", "1", NULL}},
    {"insights.overview", {"--days", "365", NULL}},
    {"insights.overview", {"--days", "366", NULL}},
    {"insights.overview", {"--days", "99999", NULL}},
    {"insights.overview", {"--days", "abc", NULL}},
    {"insights.overview", {"--days", "12x", NULL}},
    {"insights.overview", {"--days", "", NULL}},

    /* delegate.backend_exec — the command is the LAST positional, so the
       samples have to vary how many precede it and put flags after it. */
    {"delegate.backend_exec", {NULL}},
    {"delegate.backend_exec", {"ls -la", NULL}},
    {"delegate.backend_exec", {"first", "second", "ls -la", NULL}},
    {"delegate.backend_exec", {"--backend", "docker", "ls -la", NULL}},
    {"delegate.backend_exec", {"ls -la", "--backend", "docker", NULL}},
    {"delegate.backend_exec", {"--no-hibernate", "ls -la", NULL}},
    {"delegate.backend_exec", {"--backend", "", "cmd", NULL}},
    {"delegate.backend_exec", {"--task-id", "t1", "--image", "img", "--host", "h", "cmd", NULL}},
    {"delegate.backend_exec", {"", NULL}},
    {"delegate.backend_exec", {"a", "", NULL}},
    {"delegate.backend_exec", {"--no-hibernate", NULL}},

    /* provider.list — every bool flag absent, then each present, then all.
     * An unknown flag must not change the body: the server rejects what it
     * does not know, and the client inventing a field for it would be worse. */
    {"provider.list", {NULL}},
    {"provider.list", {"--json", NULL}},
    {"provider.list", {"--available", NULL}},
    {"provider.list", {"--all", "--json", NULL}},
    {"provider.list", {"--nonsense", NULL}},

    /* provider.models — a positional, with and without the bool. An empty
     * positional is not a value: both sides must drop it, or one sends
     * "name":"" and the server filters on the empty string. */
    {"provider.models", {NULL}},
    {"provider.models", {"openai", NULL}},
    {"provider.models", {"openai", "--json", NULL}},
    {"provider.models", {"", NULL}},

    /* catalog.list — a valued flag, a true_if_set and an explicit bool, which
     * render differently (absent vs true vs false) and are the likeliest thing
     * for a spec to get subtly wrong. */
    {"catalog.list", {NULL}},
    {"catalog.list", {"--json", NULL}},
    {"catalog.list", {"--open-weights", NULL}},
    {"catalog.list", {"--capability", "vision", NULL}},
    {"catalog.list", {"--capability", "vision", "--json", "--open-weights", NULL}},

    /* trigger.list — a single optional flag. */
    {"trigger.list", {NULL}},
    {"trigger.list", {"--status", "pending", NULL}},

    /* trigger.status / trigger.cancel — required, reachable as a positional OR
     * --id, and both sides must REFUSE when it is absent rather than send a
     * body without it. */
    {"trigger.status", {"tr-1", NULL}},
    {"trigger.status", {"--id", "tr-1", NULL}},
    {"trigger.status", {NULL}},
    {"trigger.cancel", {"tr-1", NULL}},
    {"trigger.cancel", {"--id", "tr-1", NULL}},
    {"trigger.cancel", {NULL}},

    /* model.episodes — optional, positional OR --agent. */
    {"model.episodes", {NULL}},
    {"model.episodes", {"claude", NULL}},
    {"model.episodes", {"--agent", "claude", NULL}},
    {"model.episodes", {"", NULL}},

    /* graph.sync_code — a plain optional positional, which does NOT fall back
     * to a flag; sampling --project proves it stays absent rather than quietly
     * acquiring the fallback that positional_or_flag has. */
    {"graph.sync_code", {NULL}},
    {"graph.sync_code", {"aimee", NULL}},
    {"graph.sync_code", {"--project", "aimee", NULL}},

    /* dogfood.report — two optional valued flags, neither with a positional
     * fallback, so a bare positional must not become either field. */
    {"dogfood.report", {NULL}},
    {"dogfood.report", {"--month", "2026-08", NULL}},
    {"dogfood.report", {"--dir", "reports/aug", NULL}},
    {"dogfood.report", {"--month", "2026-08", "--dir", "reports/aug", NULL}},
    {"dogfood.report", {"stray-positional", NULL}},

    /* The "empty":"emit" family. Every one of these gates on pos_count alone,
     * so an empty argument is a value the operator typed and is sent. The
     * empty-string samples are the whole point: without the flag each of these
     * disagreed with its marshaller on exactly that input, and on nothing
     * else. */
    {"eval.results", {NULL}},
    {"eval.results", {"suite-a", NULL}},
    {"eval.results", {"", NULL}},

    {"cert.revoke", {NULL}},
    {"cert.revoke", {"AB12", NULL}},
    {"cert.revoke", {"", NULL}},

    {"vault.capability", {NULL}},
    {"vault.capability", {"grant", NULL}},
    {"vault.capability", {"grant", "uid:1000", NULL}},
    {"vault.capability", {"", "uid:1000", NULL}},

    {"vault.delete", {"git", "token", NULL}},
    {"vault.delete", {"git", "", NULL}},
    {"vault.delete", {NULL}},

    {"vault.set", {"git", "token", "s3cret", NULL}},
    {"vault.set", {"git", "token", "", NULL}},
    {"vault.set", {"git", NULL}},

    {"vault.set_server", {"git", "token", "s3cret", NULL}},
    {"vault.set_server", {"git", "", "s3cret", NULL}},
    {"vault.set_server", {NULL}},

    /* The lenient-number family. The non-numeric samples are the point: atoi
     * turns "abc" into 0 and "12x" into 12, and a spec claiming to describe
     * these has to do the same or it diverges on the first typo. */
    {"graph.explain", {NULL}},
    {"graph.explain", {"widget", NULL}},
    {"graph.explain", {"widget", "--limit", "5", NULL}},
    {"graph.explain", {"widget", "--limit", "abc", NULL}},
    {"graph.explain", {"widget", "--limit", "12x", NULL}},

    {"aux.test", {NULL}},
    {"aux.test", {"t", NULL}},
    {"aux.test", {"t", "p", NULL}},
    {"aux.test", {"t", "p", "512", NULL}},
    {"aux.test", {"t", "p", "abc", NULL}},
    {"aux.test", {"t", "", "7", NULL}},

    {"dogfood.review", {NULL}},
    {"dogfood.review", {"--month", "2026-08", NULL}},
    {"dogfood.review", {"--limit", "9", NULL}},
    {"dogfood.review", {"--limit", "nine", NULL}},
    {"dogfood.review", {"--json", NULL}},

    {"config.get", {NULL}},
    {"config.get", {"v0", NULL}},
    {"config.get", {"", NULL}},
    {"config.set", {NULL}},
    {"config.set", {"v0", "v1", NULL}},
    {"config.set", {"", "v1", NULL}},
    {"delegate.aggregate", {NULL}},
    {"delegate.aggregate", {"v0", NULL}},
    {"delegate.aggregate", {"", NULL}},
    {"evidence.fidelity_retrieval_event", {NULL}},
    {"evidence.fidelity_retrieval_event", {"v0", NULL}},
    {"evidence.fidelity_retrieval_event", {"", NULL}},
    {"evidence.provenance_retrieval_event", {NULL}},
    {"evidence.provenance_retrieval_event", {"v0", NULL}},
    {"evidence.provenance_retrieval_event", {"", NULL}},
    {"evidence.trace_retrieval_event", {NULL}},
    {"evidence.trace_retrieval_event", {"v0", NULL}},
    {"evidence.trace_retrieval_event", {"", NULL}},
    {"index.scan", {NULL}},
    {"index.scan", {"v0", "v1", NULL}},
    {"index.scan", {"", "v1", NULL}},
    {"index.scan", {"v0", "v1", "--force", NULL}},
    {"kb.build", {NULL}},
    {"kb.build", {"--path", "x", NULL}},
    {"kb.build", {"--path", "x", "--project", "x", NULL}},
    {"kb.build", {"--path", "x", "--force", NULL}},
    {"kb.build", {"--path", "x", "--embed", "x", NULL}},
    {"kb.ingest", {NULL}},
    {"kb.ingest", {"v0", NULL}},
    {"kb.ingest", {"", NULL}},
    {"kb.ingest", {"v0", "--force", NULL}},
    {"kb.ingest", {"v0", "--embed", "x", NULL}},
    {"kb.status", {NULL}},
    {"kb.status", {"v0", NULL}},
    {"kb.status", {"", NULL}},
    {"kb.update", {NULL}},
    {"kb.update", {"v0", "v1", NULL}},
    {"kb.update", {"", "v1", NULL}},
    {"kb.update", {"v0", "v1", "--embed", "x", NULL}},
    {"curator.implements", {NULL}},
    {"curator.implements", {"v0", NULL}},
    {"curator.implements", {"", NULL}},
    {"curator.synthesize", {NULL}},
    {"curator.synthesize", {"v0", NULL}},
    {"curator.synthesize", {"", NULL}},
    {"provider.quota", {NULL}},
    {"provider.quota", {"v0", NULL}},
    {"provider.quota", {"", NULL}},
    {"provider.show", {NULL}},
    {"provider.show", {"v0", NULL}},
    {"provider.show", {"", NULL}},
    {"provider.test", {NULL}},
    {"provider.test", {"v0", NULL}},
    {"provider.test", {"", NULL}},
    {"model.add", {NULL}},
    {"model.add", {"one", NULL}},
    {"model.add", {"one", "two", NULL}},
    {"model.add", {"--flag", "v", NULL}},
    {"model.add", {"", NULL}},
    {"model.disable", {NULL}},
    {"model.disable", {"one", NULL}},
    {"model.disable", {"one", "two", NULL}},
    {"model.disable", {"--flag", "v", NULL}},
    {"model.disable", {"", NULL}},
    {"model.enable", {NULL}},
    {"model.enable", {"one", NULL}},
    {"model.enable", {"one", "two", NULL}},
    {"model.enable", {"--flag", "v", NULL}},
    {"model.enable", {"", NULL}},
    {"model.episodes", {NULL}},
    {"model.episodes", {"one", NULL}},
    {"model.episodes", {"one", "two", NULL}},
    {"model.episodes", {"--flag", "v", NULL}},
    {"model.episodes", {"", NULL}},
    {"model.list", {NULL}},
    {"model.list", {"one", NULL}},
    {"model.list", {"one", "two", NULL}},
    {"model.list", {"--flag", "v", NULL}},
    {"model.list", {"", NULL}},
    {"model.local", {NULL}},
    {"model.local", {"one", NULL}},
    {"model.local", {"one", "two", NULL}},
    {"model.local", {"--flag", "v", NULL}},
    {"model.local", {"", NULL}},
    {"model.personas", {NULL}},
    {"model.personas", {"one", NULL}},
    {"model.personas", {"one", "two", NULL}},
    {"model.personas", {"--flag", "v", NULL}},
    {"model.personas", {"", NULL}},
    {"model.probe", {NULL}},
    {"model.probe", {"one", NULL}},
    {"model.probe", {"one", "two", NULL}},
    {"model.probe", {"--flag", "v", NULL}},
    {"model.probe", {"", NULL}},
    {"model.remove", {NULL}},
    {"model.remove", {"one", NULL}},
    {"model.remove", {"one", "two", NULL}},
    {"model.remove", {"--flag", "v", NULL}},
    {"model.remove", {"", NULL}},
    {"model.roles", {NULL}},
    {"model.roles", {"one", NULL}},
    {"model.roles", {"one", "two", NULL}},
    {"model.roles", {"--flag", "v", NULL}},
    {"model.roles", {"", NULL}},
    {"cert.issue", {NULL}},
    {"cert.issue", {"v0", NULL}},
    {"cert.issue", {"", NULL}},
    {"cert.issue", {"v0", "--days", "7", NULL}},
    {"cert.issue", {"v0", "--days", "abc", NULL}},
    {"job.cancel", {NULL}},
    {"job.cancel", {"0", NULL}},
    {"job.cancel", {"", NULL}},
    {"job.cancel", {"--job-id", "7", NULL}},
    {"job.cancel", {"0", "--reason", "x", NULL}},
    {"job.status", {NULL}},
    {"job.status", {"0", NULL}},
    {"job.status", {"", NULL}},
    {"job.status", {"--job-id", "7", NULL}},
    {"job.status", {"0", "--reason", "x", NULL}},
    {"jobs.cancel", {NULL}},
    {"jobs.cancel", {"0", NULL}},
    {"jobs.cancel", {"", NULL}},
    {"jobs.cancel", {"--job-id", "7", NULL}},
    {"jobs.cancel", {"0", "--reason", "x", NULL}},
    {"jobs.logs", {NULL}},
    {"jobs.logs", {"0", NULL}},
    {"jobs.logs", {"", NULL}},
    {"jobs.logs", {"--job-id", "7", NULL}},
    {"jobs.logs", {"0", "--reason", "x", NULL}},
    {"jobs.status", {NULL}},
    {"jobs.status", {"0", NULL}},
    {"jobs.status", {"", NULL}},
    {"jobs.status", {"--job-id", "7", NULL}},
    {"jobs.status", {"0", "--reason", "x", NULL}},
    {"kb.reembed", {NULL}},
    {"kb.reembed", {"--confirm", NULL}},
    {"kb.reembed", {"--force", NULL}},
    {"kb.reembed", {"--dry-run", NULL}},
    {"kb.reembed", {"--clear-maintenance", NULL}},
    {"kb.reembed", {"--target-dim", "7", NULL}},
    {"kb.reembed", {"--target-dim", "abc", NULL}},
    {"rules.delete", {NULL}},
    {"rules.delete", {"0", NULL}},
    {"rules.delete", {"", NULL}},
    {"rules.delete", {"--id", "7", NULL}},
    {"job.start", {NULL}},
    {"job.start", {"0", NULL}},
    {"job.start", {"", NULL}},
    {"job.start", {"--plan-id", "7", NULL}},
    {"job.start", {"0", "--parallel", "7", NULL}},
    {"job.start", {"0", "--parallel", "0", NULL}},
    {"job.start", {"0", "--parallel", "abc", NULL}},
    {"session.list", {NULL}},
    {"session.list", {"--limit", "7", NULL}},
    {"session.list", {"--limit", "0", NULL}},
    {"session.list", {"--limit", "abc", NULL}},
    {"curator.contradictions", {NULL}},
    {"curator.contradictions", {"--limit", "7", NULL}},
    {"curator.contradictions", {"--limit", "0", NULL}},
    {"curator.contradictions", {"--limit", "abc", NULL}},
    {"job.list", {NULL}},
    {"job.list", {"--limit", "7", NULL}},
    {"job.list", {"--limit", "0", NULL}},
    {"job.list", {"--limit", "abc", NULL}},
    {"jobs.list", {NULL}},
    {"jobs.list", {"--limit", "7", NULL}},
    {"jobs.list", {"--limit", "0", NULL}},
    {"jobs.list", {"--limit", "abc", NULL}},
    {"notes.search", {NULL}},
    {"notes.search", {"v0", NULL}},
    {"notes.search", {"", NULL}},
    {"notes.search", {"--query", "7", NULL}},
    {"notes.search", {"v0", "--limit", "7", NULL}},
    {"notes.search", {"v0", "--limit", "0", NULL}},
    {"notes.search", {"v0", "--limit", "abc", NULL}},
    {"cron.show", {NULL}},
    {"cron.show", {"j1", NULL}},
    {"cron.show", {"--id", "j1", NULL}},
    {"cron.show", {"", NULL}},
    {"cron.show", {"j1", "--limit", "5", NULL}},
    {"cron.show", {"j1", "--limit", "abc", NULL}},
    {"cron.run", {NULL}},
    {"cron.run", {"j1", NULL}},
    {"cron.run", {"--id", "j1", NULL}},
    {"cron.run", {"", NULL}},
    {"cron.run", {"j1", "--limit", "5", NULL}},
    {"cron.run", {"j1", "--limit", "abc", NULL}},
    {"cron.remove", {NULL}},
    {"cron.remove", {"j1", NULL}},
    {"cron.remove", {"--id", "j1", NULL}},
    {"cron.remove", {"", NULL}},
    {"cron.remove", {"j1", "--limit", "5", NULL}},
    {"cron.remove", {"j1", "--limit", "abc", NULL}},
    {"cron.history", {NULL}},
    {"cron.history", {"j1", NULL}},
    {"cron.history", {"--id", "j1", NULL}},
    {"cron.history", {"", NULL}},
    {"cron.history", {"j1", "--limit", "5", NULL}},
    {"cron.history", {"j1", "--limit", "abc", NULL}},
    {"pipeline.advance", {NULL}},
    {"pipeline.advance", {"7", NULL}},
    {"pipeline.advance", {"abc", NULL}},
    {"pipeline.advance", {"--state", "open", NULL}},
    {"pipeline.advance", {"--verdict", "pass", NULL}},
    {"pipeline.advance", {"7", "--artifact", "a", NULL}},
    {"pipeline.advance", {"7", "--remote", "r", NULL}},
    {"pipeline.cancel", {NULL}},
    {"pipeline.cancel", {"7", NULL}},
    {"pipeline.cancel", {"abc", NULL}},
    {"pipeline.cancel", {"--state", "open", NULL}},
    {"pipeline.cancel", {"--verdict", "pass", NULL}},
    {"pipeline.cancel", {"7", "--artifact", "a", NULL}},
    {"pipeline.cancel", {"7", "--remote", "r", NULL}},
    {"pipeline.gate", {NULL}},
    {"pipeline.gate", {"7", NULL}},
    {"pipeline.gate", {"abc", NULL}},
    {"pipeline.gate", {"--state", "open", NULL}},
    {"pipeline.gate", {"--verdict", "pass", NULL}},
    {"pipeline.gate", {"7", "--artifact", "a", NULL}},
    {"pipeline.gate", {"7", "--remote", "r", NULL}},
    {"pipeline.list", {NULL}},
    {"pipeline.list", {"7", NULL}},
    {"pipeline.list", {"abc", NULL}},
    {"pipeline.list", {"--state", "open", NULL}},
    {"pipeline.list", {"--verdict", "pass", NULL}},
    {"pipeline.list", {"7", "--artifact", "a", NULL}},
    {"pipeline.list", {"7", "--remote", "r", NULL}},
    {"pipeline.resume", {NULL}},
    {"pipeline.resume", {"7", NULL}},
    {"pipeline.resume", {"abc", NULL}},
    {"pipeline.resume", {"--state", "open", NULL}},
    {"pipeline.resume", {"--verdict", "pass", NULL}},
    {"pipeline.resume", {"7", "--artifact", "a", NULL}},
    {"pipeline.resume", {"7", "--remote", "r", NULL}},
    {"pipeline.show", {NULL}},
    {"pipeline.show", {"7", NULL}},
    {"pipeline.show", {"abc", NULL}},
    {"pipeline.show", {"--state", "open", NULL}},
    {"pipeline.show", {"--verdict", "pass", NULL}},
    {"pipeline.show", {"7", "--artifact", "a", NULL}},
    {"pipeline.show", {"7", "--remote", "r", NULL}},
    {"pipeline.status", {NULL}},
    {"pipeline.status", {"7", NULL}},
    {"pipeline.status", {"abc", NULL}},
    {"pipeline.status", {"--state", "open", NULL}},
    {"pipeline.status", {"--verdict", "pass", NULL}},
    {"pipeline.status", {"7", "--artifact", "a", NULL}},
    {"pipeline.status", {"7", "--remote", "r", NULL}},
    {"api.enable", {NULL}},
    {"api.enable", {"--vscode", NULL}},
    {"api.enable", {"--port", "7", NULL}},
    {"api.enable", {"--port", "0", NULL}},
    {"api.enable", {"--port", "abc", NULL}},
    {"api.enable", {"--rate-limit", "7", NULL}},
    {"api.enable", {"--rate-limit", "0", NULL}},
    {"api.enable", {"--rate-limit", "abc", NULL}},
    {"session.brief", {NULL}},
    {"session.brief", {"v0", NULL}},
    {"session.brief", {"", NULL}},
    {"session.brief", {"v0", "--list", NULL}},
    {"workspace.get", {NULL}},
    {"workspace.get", {"one", NULL}},
    {"workspace.get", {"one", "two", NULL}},
    {"workspace.get", {"--flag", "v", NULL}},
    {"workspace.get", {"", NULL}},
    {"workspace.remove", {NULL}},
    {"workspace.remove", {"one", NULL}},
    {"workspace.remove", {"one", "two", NULL}},
    {"workspace.remove", {"--flag", "v", NULL}},
    {"workspace.remove", {"", NULL}},
    {"index.deps", {NULL}},
    {"index.deps", {"v0", NULL}},
    {"index.deps", {"", NULL}},
    {"index.deps", {"v0", "--tier", "x", NULL}},
    {"index.deps", {"v0", "--review", NULL}},
    {"index.deps", {"v0", "--reverse", NULL}},
    {"index.deps", {"v0", "--dry-run", NULL}},
    {"trajectory.export", {NULL}},
    {"trajectory.export", {"s1", NULL}},
    {"trajectory.export", {"--session", "s1", NULL}},
    {"trajectory.export", {"s1", "--no-compress", NULL}},
    {"trajectory.export", {"s1", "--max-result-bytes", "0", NULL}},
    {"trajectory.export", {"s1", "--max-result-bytes", "abc", NULL}},
    {"trajectory.export", {"s1", "--max-result-bytes", "99", NULL}},
    {"trajectory.batch", {NULL}},
    {"trajectory.batch", {"--tasks", "c.jsonl", NULL}},
    {"trajectory.batch", {"--tasks", "c.jsonl", "--no-compress", NULL}},
    {"trajectory.batch", {"--tasks", "c.jsonl", "--out", "d", NULL}},
    {"trajectory.batch", {"--tasks", "c.jsonl", "--max-result-bytes", "0", NULL}},
    {"trajectory.batch", {"--tasks", "c.jsonl", "--toolset-dist", "research", NULL}},
    {"session.close", {NULL}},
    {"session.close", {"v0", NULL}},
    {"session.close", {"", NULL}},
    {"session.close", {"--session", "viaflag", NULL}},
    {"session.get", {NULL}},
    {"session.get", {"v0", NULL}},
    {"session.get", {"", NULL}},
    {"session.get", {"--session", "viaflag", NULL}},

    /* Adversarial samples, NOT derived from the specs.
     *
     * Every other sample here is generated from the spec under test, so it
     * can only probe rules the spec already mentions. A marshaller rule the
     * spec OMITS is invisible to those: memory.user_capture refuses a key
     * over 512 characters, and no spec-derived sample is ever that long.
     *
     * These come from the marshallers' failure modes instead and are applied
     * to every shipped spec. They cannot catch everything, but a gate whose
     * inputs are all suggested by the thing it is testing is a gate with a
     * hole exactly the shape of what was forgotten. */
    {"api.enable",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"api.enable", {"--", "v", NULL}},
    {"api.enable", {"-notaflag", NULL}},
    {"api.enable", {"a", "b", "c", "d", "e", NULL}},
    {"aux.test",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"aux.test", {"--", "v", NULL}},
    {"aux.test", {"-notaflag", NULL}},
    {"aux.test", {"a", "b", "c", "d", "e", NULL}},
    {"catalog.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"catalog.list", {"--", "v", NULL}},
    {"catalog.list", {"-notaflag", NULL}},
    {"catalog.list", {"a", "b", "c", "d", "e", NULL}},
    {"cert.issue",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"cert.issue", {"--", "v", NULL}},
    {"cert.issue", {"-notaflag", NULL}},
    {"cert.issue", {"a", "b", "c", "d", "e", NULL}},
    {"cert.revoke",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"cert.revoke", {"--", "v", NULL}},
    {"cert.revoke", {"-notaflag", NULL}},
    {"cert.revoke", {"a", "b", "c", "d", "e", NULL}},
    {"config.get",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"config.get", {"--", "v", NULL}},
    {"config.get", {"-notaflag", NULL}},
    {"config.get", {"a", "b", "c", "d", "e", NULL}},
    {"config.set",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"config.set", {"--", "v", NULL}},
    {"config.set", {"-notaflag", NULL}},
    {"config.set", {"a", "b", "c", "d", "e", NULL}},
    {"cron.history",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"cron.history", {"--", "v", NULL}},
    {"cron.history", {"-notaflag", NULL}},
    {"cron.history", {"a", "b", "c", "d", "e", NULL}},
    {"cron.remove",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"cron.remove", {"--", "v", NULL}},
    {"cron.remove", {"-notaflag", NULL}},
    {"cron.remove", {"a", "b", "c", "d", "e", NULL}},
    {"cron.run",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"cron.run", {"--", "v", NULL}},
    {"cron.run", {"-notaflag", NULL}},
    {"cron.run", {"a", "b", "c", "d", "e", NULL}},
    {"cron.show",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"cron.show", {"--", "v", NULL}},
    {"cron.show", {"-notaflag", NULL}},
    {"cron.show", {"a", "b", "c", "d", "e", NULL}},
    {"curator.contradictions",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"curator.contradictions", {"--", "v", NULL}},
    {"curator.contradictions", {"-notaflag", NULL}},
    {"curator.contradictions", {"a", "b", "c", "d", "e", NULL}},
    {"curator.implements",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"curator.implements", {"--", "v", NULL}},
    {"curator.implements", {"-notaflag", NULL}},
    {"curator.implements", {"a", "b", "c", "d", "e", NULL}},
    {"curator.synthesize",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"curator.synthesize", {"--", "v", NULL}},
    {"curator.synthesize", {"-notaflag", NULL}},
    {"curator.synthesize", {"a", "b", "c", "d", "e", NULL}},
    {"delegate.aggregate",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"delegate.aggregate", {"--", "v", NULL}},
    {"delegate.aggregate", {"-notaflag", NULL}},
    {"delegate.aggregate", {"a", "b", "c", "d", "e", NULL}},
    {"dogfood.report",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"dogfood.report", {"--", "v", NULL}},
    {"dogfood.report", {"-notaflag", NULL}},
    {"dogfood.report", {"a", "b", "c", "d", "e", NULL}},
    {"dogfood.review",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"dogfood.review", {"--", "v", NULL}},
    {"dogfood.review", {"-notaflag", NULL}},
    {"dogfood.review", {"a", "b", "c", "d", "e", NULL}},
    {"eval.results",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"eval.results", {"--", "v", NULL}},
    {"eval.results", {"-notaflag", NULL}},
    {"eval.results", {"a", "b", "c", "d", "e", NULL}},
    {"evidence.fidelity_retrieval_event",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"evidence.fidelity_retrieval_event", {"--", "v", NULL}},
    {"evidence.fidelity_retrieval_event", {"-notaflag", NULL}},
    {"evidence.fidelity_retrieval_event", {"a", "b", "c", "d", "e", NULL}},
    {"evidence.provenance_retrieval_event",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"evidence.provenance_retrieval_event", {"--", "v", NULL}},
    {"evidence.provenance_retrieval_event", {"-notaflag", NULL}},
    {"evidence.provenance_retrieval_event", {"a", "b", "c", "d", "e", NULL}},
    {"evidence.trace_retrieval_event",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"evidence.trace_retrieval_event", {"--", "v", NULL}},
    {"evidence.trace_retrieval_event", {"-notaflag", NULL}},
    {"evidence.trace_retrieval_event", {"a", "b", "c", "d", "e", NULL}},
    {"graph.explain",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"graph.explain", {"--", "v", NULL}},
    {"graph.explain", {"-notaflag", NULL}},
    {"graph.explain", {"a", "b", "c", "d", "e", NULL}},
    {"graph.sync_code",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"graph.sync_code", {"--", "v", NULL}},
    {"graph.sync_code", {"-notaflag", NULL}},
    {"graph.sync_code", {"a", "b", "c", "d", "e", NULL}},
    {"index.deps",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"index.deps", {"--", "v", NULL}},
    {"index.deps", {"-notaflag", NULL}},
    {"index.deps", {"a", "b", "c", "d", "e", NULL}},
    {"index.scan",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"index.scan", {"--", "v", NULL}},
    {"index.scan", {"-notaflag", NULL}},
    {"index.scan", {"a", "b", "c", "d", "e", NULL}},
    {"job.cancel",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"job.cancel", {"--", "v", NULL}},
    {"job.cancel", {"-notaflag", NULL}},
    {"job.cancel", {"a", "b", "c", "d", "e", NULL}},
    {"job.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"job.list", {"--", "v", NULL}},
    {"job.list", {"-notaflag", NULL}},
    {"job.list", {"a", "b", "c", "d", "e", NULL}},
    {"job.start",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"job.start", {"--", "v", NULL}},
    {"job.start", {"-notaflag", NULL}},
    {"job.start", {"a", "b", "c", "d", "e", NULL}},
    {"job.status",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"job.status", {"--", "v", NULL}},
    {"job.status", {"-notaflag", NULL}},
    {"job.status", {"a", "b", "c", "d", "e", NULL}},
    {"jobs.cancel",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"jobs.cancel", {"--", "v", NULL}},
    {"jobs.cancel", {"-notaflag", NULL}},
    {"jobs.cancel", {"a", "b", "c", "d", "e", NULL}},
    {"jobs.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"jobs.list", {"--", "v", NULL}},
    {"jobs.list", {"-notaflag", NULL}},
    {"jobs.list", {"a", "b", "c", "d", "e", NULL}},
    {"jobs.logs",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"jobs.logs", {"--", "v", NULL}},
    {"jobs.logs", {"-notaflag", NULL}},
    {"jobs.logs", {"a", "b", "c", "d", "e", NULL}},
    {"jobs.status",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"jobs.status", {"--", "v", NULL}},
    {"jobs.status", {"-notaflag", NULL}},
    {"jobs.status", {"a", "b", "c", "d", "e", NULL}},
    {"kb.build",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"kb.build", {"--", "v", NULL}},
    {"kb.build", {"-notaflag", NULL}},
    {"kb.build", {"a", "b", "c", "d", "e", NULL}},
    {"kb.ingest",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"kb.ingest", {"--", "v", NULL}},
    {"kb.ingest", {"-notaflag", NULL}},
    {"kb.ingest", {"a", "b", "c", "d", "e", NULL}},
    {"kb.reembed",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"kb.reembed", {"--", "v", NULL}},
    {"kb.reembed", {"-notaflag", NULL}},
    {"kb.reembed", {"a", "b", "c", "d", "e", NULL}},
    {"kb.status",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"kb.status", {"--", "v", NULL}},
    {"kb.status", {"-notaflag", NULL}},
    {"kb.status", {"a", "b", "c", "d", "e", NULL}},
    {"kb.update",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"kb.update", {"--", "v", NULL}},
    {"kb.update", {"-notaflag", NULL}},
    {"kb.update", {"a", "b", "c", "d", "e", NULL}},
    {"model.add",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.add", {"--", "v", NULL}},
    {"model.add", {"-notaflag", NULL}},
    {"model.add", {"a", "b", "c", "d", "e", NULL}},
    {"model.disable",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.disable", {"--", "v", NULL}},
    {"model.disable", {"-notaflag", NULL}},
    {"model.disable", {"a", "b", "c", "d", "e", NULL}},
    {"model.enable",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.enable", {"--", "v", NULL}},
    {"model.enable", {"-notaflag", NULL}},
    {"model.enable", {"a", "b", "c", "d", "e", NULL}},
    {"model.episodes",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.episodes", {"--", "v", NULL}},
    {"model.episodes", {"-notaflag", NULL}},
    {"model.episodes", {"a", "b", "c", "d", "e", NULL}},
    {"model.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.list", {"--", "v", NULL}},
    {"model.list", {"-notaflag", NULL}},
    {"model.list", {"a", "b", "c", "d", "e", NULL}},
    {"model.local",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.local", {"--", "v", NULL}},
    {"model.local", {"-notaflag", NULL}},
    {"model.local", {"a", "b", "c", "d", "e", NULL}},
    {"model.personas",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.personas", {"--", "v", NULL}},
    {"model.personas", {"-notaflag", NULL}},
    {"model.personas", {"a", "b", "c", "d", "e", NULL}},
    {"model.probe",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.probe", {"--", "v", NULL}},
    {"model.probe", {"-notaflag", NULL}},
    {"model.probe", {"a", "b", "c", "d", "e", NULL}},
    {"model.remove",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.remove", {"--", "v", NULL}},
    {"model.remove", {"-notaflag", NULL}},
    {"model.remove", {"a", "b", "c", "d", "e", NULL}},
    {"model.roles",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"model.roles", {"--", "v", NULL}},
    {"model.roles", {"-notaflag", NULL}},
    {"model.roles", {"a", "b", "c", "d", "e", NULL}},
    {"notes.search",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"notes.search", {"--", "v", NULL}},
    {"notes.search", {"-notaflag", NULL}},
    {"notes.search", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.advance",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.advance", {"--", "v", NULL}},
    {"pipeline.advance", {"-notaflag", NULL}},
    {"pipeline.advance", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.cancel",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.cancel", {"--", "v", NULL}},
    {"pipeline.cancel", {"-notaflag", NULL}},
    {"pipeline.cancel", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.gate",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.gate", {"--", "v", NULL}},
    {"pipeline.gate", {"-notaflag", NULL}},
    {"pipeline.gate", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.list", {"--", "v", NULL}},
    {"pipeline.list", {"-notaflag", NULL}},
    {"pipeline.list", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.resume",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.resume", {"--", "v", NULL}},
    {"pipeline.resume", {"-notaflag", NULL}},
    {"pipeline.resume", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.show",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.show", {"--", "v", NULL}},
    {"pipeline.show", {"-notaflag", NULL}},
    {"pipeline.show", {"a", "b", "c", "d", "e", NULL}},
    {"pipeline.status",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"pipeline.status", {"--", "v", NULL}},
    {"pipeline.status", {"-notaflag", NULL}},
    {"pipeline.status", {"a", "b", "c", "d", "e", NULL}},
    {"provider.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"provider.list", {"--", "v", NULL}},
    {"provider.list", {"-notaflag", NULL}},
    {"provider.list", {"a", "b", "c", "d", "e", NULL}},
    {"provider.models",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"provider.models", {"--", "v", NULL}},
    {"provider.models", {"-notaflag", NULL}},
    {"provider.models", {"a", "b", "c", "d", "e", NULL}},
    {"provider.quota",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"provider.quota", {"--", "v", NULL}},
    {"provider.quota", {"-notaflag", NULL}},
    {"provider.quota", {"a", "b", "c", "d", "e", NULL}},
    {"provider.show",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"provider.show", {"--", "v", NULL}},
    {"provider.show", {"-notaflag", NULL}},
    {"provider.show", {"a", "b", "c", "d", "e", NULL}},
    {"provider.test",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"provider.test", {"--", "v", NULL}},
    {"provider.test", {"-notaflag", NULL}},
    {"provider.test", {"a", "b", "c", "d", "e", NULL}},
    {"rules.delete",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"rules.delete", {"--", "v", NULL}},
    {"rules.delete", {"-notaflag", NULL}},
    {"rules.delete", {"a", "b", "c", "d", "e", NULL}},
    {"session.brief",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"session.brief", {"--", "v", NULL}},
    {"session.brief", {"-notaflag", NULL}},
    {"session.brief", {"a", "b", "c", "d", "e", NULL}},
    {"session.close",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"session.close", {"--", "v", NULL}},
    {"session.close", {"-notaflag", NULL}},
    {"session.close", {"a", "b", "c", "d", "e", NULL}},
    {"session.get",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"session.get", {"--", "v", NULL}},
    {"session.get", {"-notaflag", NULL}},
    {"session.get", {"a", "b", "c", "d", "e", NULL}},
    {"session.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"session.list", {"--", "v", NULL}},
    {"session.list", {"-notaflag", NULL}},
    {"session.list", {"a", "b", "c", "d", "e", NULL}},
    {"trajectory.batch",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"trajectory.batch", {"--", "v", NULL}},
    {"trajectory.batch", {"-notaflag", NULL}},
    {"trajectory.batch", {"a", "b", "c", "d", "e", NULL}},
    {"trajectory.export",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"trajectory.export", {"--", "v", NULL}},
    {"trajectory.export", {"-notaflag", NULL}},
    {"trajectory.export", {"a", "b", "c", "d", "e", NULL}},
    {"trigger.cancel",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"trigger.cancel", {"--", "v", NULL}},
    {"trigger.cancel", {"-notaflag", NULL}},
    {"trigger.cancel", {"a", "b", "c", "d", "e", NULL}},
    {"trigger.list",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"trigger.list", {"--", "v", NULL}},
    {"trigger.list", {"-notaflag", NULL}},
    {"trigger.list", {"a", "b", "c", "d", "e", NULL}},
    {"trigger.status",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"trigger.status", {"--", "v", NULL}},
    {"trigger.status", {"-notaflag", NULL}},
    {"trigger.status", {"a", "b", "c", "d", "e", NULL}},
    {"vault.capability",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"vault.capability", {"--", "v", NULL}},
    {"vault.capability", {"-notaflag", NULL}},
    {"vault.capability", {"a", "b", "c", "d", "e", NULL}},
    {"vault.delete",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"vault.delete", {"--", "v", NULL}},
    {"vault.delete", {"-notaflag", NULL}},
    {"vault.delete", {"a", "b", "c", "d", "e", NULL}},
    {"vault.set",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"vault.set", {"--", "v", NULL}},
    {"vault.set", {"-notaflag", NULL}},
    {"vault.set", {"a", "b", "c", "d", "e", NULL}},
    {"vault.set_server",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"vault.set_server", {"--", "v", NULL}},
    {"vault.set_server", {"-notaflag", NULL}},
    {"vault.set_server", {"a", "b", "c", "d", "e", NULL}},
    {"workspace.get",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"workspace.get", {"--", "v", NULL}},
    {"workspace.get", {"-notaflag", NULL}},
    {"workspace.get", {"a", "b", "c", "d", "e", NULL}},
    {"workspace.remove",
     {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      NULL}},
    {"workspace.remove", {"--", "v", NULL}},
    {"workspace.remove", {"-notaflag", NULL}},
    {"workspace.remove", {"a", "b", "c", "d", "e", NULL}},

    /* BOTH sources supplied at once.
     *
     * positional_or_flag and flag_or_positional agree on every sample that
     * gives only one of them; they differ ONLY here. kb.build had the order
     * backwards and survived hundreds of samples because none supplied both.
     * These make the next such error fail directly rather than by luck. */
    {"kb.build", {"FROMPOS", "--path", "FROMFLAG", NULL}},
    {"job.cancel", {"FROMPOS", "--job-id", "FROMFLAG", NULL}},
    {"job.status", {"FROMPOS", "--job-id", "FROMFLAG", NULL}},
    {"jobs.cancel", {"FROMPOS", "--job-id", "FROMFLAG", NULL}},
    {"jobs.logs", {"FROMPOS", "--job-id", "FROMFLAG", NULL}},
    {"jobs.status", {"FROMPOS", "--job-id", "FROMFLAG", NULL}},
    {"rules.delete", {"FROMPOS", "--id", "FROMFLAG", NULL}},
    {"job.start", {"FROMPOS", "--plan-id", "FROMFLAG", NULL}},
    {"notes.search", {"FROMPOS", "--query", "FROMFLAG", NULL}},
    {"cron.show", {"FROMPOS", "--id", "FROMFLAG", NULL}},
    {"cron.run", {"FROMPOS", "--id", "FROMFLAG", NULL}},
    {"cron.remove", {"FROMPOS", "--id", "FROMFLAG", NULL}},
    {"cron.history", {"FROMPOS", "--id", "FROMFLAG", NULL}},
    {"session.brief", {"FROMPOS", "--session", "FROMFLAG", NULL}},
    {"trajectory.export", {"FROMPOS", "--session", "FROMFLAG", NULL}},
    {"session.close", {"FROMPOS", "--session", "FROMFLAG", NULL}},
    {"session.get", {"FROMPOS", "--session", "FROMFLAG", NULL}},
    {"memory.delete", {NULL}},
    {"memory.delete", {"7", NULL}},
    {"memory.delete", {"abc", NULL}},
    {"memory.delete", {"--json", NULL}},
    {"memory.delete", {"--json", "7", NULL}},
    {"memory.delete", {"", NULL}},
    {"memory.delete", {"-x", NULL}},
    {"provider.set", {NULL}},
    {"provider.set", {"7", NULL}},
    {"provider.set", {"abc", NULL}},
    {"provider.set", {"--json", NULL}},
    {"provider.set", {"--json", "7", NULL}},
    {"provider.set", {"", NULL}},
    {"provider.set", {"-x", NULL}},
    {"workspace.add", {NULL}},
    {"workspace.add", {"/p", NULL}},
    {"workspace.add", {"/p", "--provider", "mirror", NULL}},
    {"workspace.add", {"/p", "--no-scan", NULL}},
    {"workspace.add", {"/p", "--remote", "r", "--head", "h", NULL}},
    {"workspace.add", {"", NULL}},
    {"workspace.add", {"/p", "--provider", "", NULL}},
    {"cron.add", {NULL}},
    {"cron.add", {"j1", NULL}},
    {"cron.add", {"j1", "--schedule", "5m", NULL}},
    {"cron.add", {"j1", "--schedule", "5m", "--skill", "a", NULL}},
    {"cron.add", {"j1", "--schedule", "5m", "--skill", "a", "--skill", "b", NULL}},
    {"cron.add", {"j1", "--schedule", "5m", "--disabled", NULL}},
    {"cron.add", {"j1", "--schedule", "5m", "--only-if-changed", "--pre-wake-gate", NULL}},
    {"cron.add", {"j1", "--schedule", "5m", "--mode", "llm", "--target", "t", NULL}},
    {"mcp.recheck", {NULL}},
    {"mcp.recheck", {"srv", NULL}},
    {"mcp.recheck", {"--json", NULL}},
    {"mcp.recheck", {"-x", NULL}},
    {"mcp.recheck", {"", NULL}},
    {"mcp.recheck", {"srv", "extra", NULL}},
    {"toolset.show", {NULL}},
    {"toolset.show", {"core", NULL}},
    {"toolset.show", {"", NULL}},
    {"toolset.show", {"--json", NULL}},
    {"toolset.show", {"core", "extra", NULL}},
    {"toolset.resolve", {NULL}},
    {"toolset.resolve", {"core", NULL}},
    {"toolset.resolve", {"", NULL}},
    {"toolset.resolve", {"--json", NULL}},
    {"toolset.resolve", {"core", "extra", NULL}},
    {"dogfood.tag", {NULL}},
    {"dogfood.tag", {"r1", NULL}},
    {"dogfood.tag", {"r1", "--surprise", NULL}},
    {"dogfood.tag", {"r1", "--no-surprise", NULL}},
    {"dogfood.tag", {"r1", "--surprise", "--no-surprise", NULL}},
    {"dogfood.tag", {"r1", "--richness", "3", NULL}},
    {"dogfood.tag", {"r1", "--richness", "abc", NULL}},
    {"dogfood.tag", {"r1", "--outcome", "", NULL}},
    {"dogfood.tag", {"", NULL}},
};

static const char *spec_for(const char *method)
{
   for (size_t i = 0; i < sizeof(SHIPPED) / sizeof(SHIPPED[0]); i++)
      if (strcmp(SHIPPED[i].method, method) == 0)
         return SHIPPED[i].spec;
   return NULL;
}

/* Every shipped spec must have samples. Otherwise a spec added tomorrow ships
 * to every client without one line of evidence that it builds the same request
 * the marshaller does — which is the whole property this file exists for. */
static void test_every_shipped_spec_is_sampled(void)
{
   for (size_t i = 0; i < sizeof(SHIPPED) / sizeof(SHIPPED[0]); i++)
   {
      int found = 0;
      for (size_t j = 0; j < sizeof(SAMPLES) / sizeof(SAMPLES[0]) && !found; j++)
         if (strcmp(SAMPLES[j].method, SHIPPED[i].method) == 0)
            found = 1;
      if (!found)
         fail("unproven spec", SHIPPED[i].method);
   }
}

static int argv_len(const char *const *argv)
{
   int n = 0;
   while (argv[n])
      n++;
   return n;
}

/* Compare the compiled marshaller's body with the interpreter's, rendered. */
static void check_same(const sample_t *s)
{
   char *argv_buf[8];
   int argc = argv_len(s->argv);
   for (int i = 0; i < argc; i++)
      argv_buf[i] = (char *)s->argv[i];

   const char *spec_json = spec_for(s->method);
   if (!spec_json)
   {
      /* A sample for a method the server does not serve a spec for. Not
       * harmless: it reads as coverage while proving nothing. */
      fail(s->method, "sampled, but no shipped spec has this method");
      return;
   }
   cJSON *spec_doc = cJSON_Parse(spec_json);
   if (!spec_doc)
   {
      fail(s->method, "spec is not valid JSON");
      return;
   }
   if (!cli_argspec_supported(spec_doc))
   {
      fail(s->method, "spec uses something the interpreter does not know");
      cJSON_Delete(spec_doc);
      return;
   }

   cJSON *from_code = marshal_request(s->method, argc, argv_buf);
   cJSON *from_spec = cli_argspec_build(s->method, spec_doc, argc, argv_buf);

   /* Both refusing is agreement: a required argument was missing and neither
    * would have sent anything. */
   if (!from_code && !from_spec)
   {
      cJSON_Delete(spec_doc);
      return;
   }
   if (!from_code || !from_spec)
   {
      char detail[512];
      snprintf(detail, sizeof(detail), "one side refused (compiled=%s spec=%s) for argv[0]=%s",
               from_code ? "built" : "NULL", from_spec ? "built" : "NULL",
               argc > 0 ? s->argv[0] : "(none)");
      fail(s->method, detail);
   }
   else
   {
      char *a = cJSON_PrintUnformatted(from_code);
      char *b = cJSON_PrintUnformatted(from_spec);
      if (!a || !b || strcmp(a, b) != 0)
      {
         char detail[1024];
         snprintf(detail, sizeof(detail), "bodies differ\n    compiled: %s\n    spec:     %s",
                  a ? a : "(null)", b ? b : "(null)");
         fail(s->method, detail);
      }
      free(a);
      free(b);
   }
   cJSON_Delete(from_code);
   cJSON_Delete(from_spec);
   cJSON_Delete(spec_doc);
}

/* ---- spec validation ---------------------------------------------------- */

static void test_refuses_what_it_cannot_build(void)
{
   /* Each of these names something this build does not understand. Accepting
    * any of them would mean building a body from a spec written for a client
    * that knows more — the one way a served spec can be actively dangerous. */
   static const char *const bad[] = {
       "{\"fields\":[{\"json\":\"x\",\"from\":\"read_file\",\"path\":\"/etc/passwd\"}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"env\",\"var\":\"HOME\"}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"flag\",\"flag\":\"x\",\"type\":\"regex\"}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"flag\"}]}",       /* no flag named   */
       "{\"fields\":[{\"json\":\"x\",\"from\":\"positional\"}]}", /* no index        */
       "{\"fields\":[{\"from\":\"flag\",\"flag\":\"x\"}]}",       /* no json name    */
       "{\"fields\":[{\"json\":\"\",\"from\":\"flag\",\"flag\":\"x\"}]}",
       "{\"fields\":{\"json\":\"x\"}}", /* fields not array*/
       "{\"bool_flags\":\"json\"}",     /* bools not array */
       "{\"fields\":[{\"json\":\"x\",\"from\":\"positional\",\"index\":-1}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"positional\",\"index\":9999}]}",
       "[]",
       "\"none\"",
       NULL,
   };
   for (int i = 0; bad[i]; i++)
   {
      cJSON *doc = cJSON_Parse(bad[i]);
      if (!doc)
      {
         fail("spec-validation", bad[i]);
         continue;
      }
      if (cli_argspec_supported(doc))
         fail("spec-validation accepted an unbuildable spec", bad[i]);
      /* And the build path must agree with the validator, not just the
       * validator alone: a caller that skipped the check must still get NULL. */
      char *argv[] = {(char *)"x"};
      cJSON *req = cli_argspec_build("some.method", doc, 1, argv);
      if (req)
      {
         fail("build accepted an unbuildable spec", bad[i]);
         cJSON_Delete(req);
      }
      cJSON_Delete(doc);
   }

   /* NULL and a non-object are refused without crashing. */
   if (cli_argspec_supported(NULL))
      fail("spec-validation", "accepted NULL");
   if (cli_argspec_build("m", NULL, 0, NULL))
      fail("build", "accepted NULL spec");

   /* An empty spec is legal — it is the no-argument case written out — and
    * builds the bare method envelope. */
   cJSON *empty = cJSON_Parse("{}");
   cJSON *req = cli_argspec_build("some.method", empty, 0, NULL);
   if (!req)
      fail("build", "refused an empty spec, which is the no-argument case");
   else
   {
      const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "method");
      if (!cJSON_IsString(m) || strcmp(m->valuestring, "some.method") != 0)
         fail("build", "empty spec did not carry the method");
      cJSON_Delete(req);
   }
   cJSON_Delete(empty);
}

static void test_number_is_refused_not_coerced(void)
{
   /* A field the spec calls a number must be refused when it is not one. The
    * compiled marshallers already refuse rather than round for exactly this
    * reason (see kb.grant's team_id), and a served spec must not be the softer
    * path into the same request. */
   cJSON *spec = cJSON_Parse("{\"usage\":\"u\",\"fields\":[{\"json\":\"n\",\"from\":\"flag\","
                             "\"flag\":\"n\",\"type\":\"number\",\"required\":true}]}");
   char *bad[] = {(char *)"--n", (char *)"12x"};
   cJSON *req = cli_argspec_build("m", spec, 2, bad);
   if (req)
   {
      fail("number", "coerced '12x' instead of refusing it");
      cJSON_Delete(req);
   }
   char *good[] = {(char *)"--n", (char *)"12"};
   req = cli_argspec_build("m", spec, 2, good);
   if (!req)
      fail("number", "refused a valid number");
   else
   {
      const cJSON *n = cJSON_GetObjectItemCaseSensitive(req, "n");
      if (!cJSON_IsNumber(n) || n->valuedouble != 12)
         fail("number", "did not emit 12 as a JSON number");
      cJSON_Delete(req);
   }
   cJSON_Delete(spec);
}

/* A positional join must carry the whole argument, however long it is. This was
 * a fixed 8 KiB buffer whose loop stopped once the next word would not fit, so
 * `aimee memory store <key> <long note>` silently dropped the tail -- and when
 * the first word alone exceeded the buffer the join came out EMPTY, which the
 * server then refused as "requires a non-empty key and content", blaming the
 * caller for text the client had discarded. Measured boundary before the fix:
 * 8191 bytes stored, 8192 reported as empty. */
static void test_positional_join_is_not_truncated(void)
{
   cJSON *spec = cJSON_Parse("{\"usage\":\"u\",\"fields\":["
                             "{\"json\":\"key\",\"from\":\"positional\",\"index\":0},"
                             "{\"json\":\"content\",\"from\":\"positional_join\","
                             "\"from_index\":1}]}");
   if (!spec)
   {
      fail("join", "could not parse the probe spec");
      return;
   }

   const size_t sizes[] = {8191, 8192, 40000};
   for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
   {
      const size_t n = sizes[s];
      char *big = malloc(n + 1);
      if (!big)
      {
         fail("join", "allocation for the probe argument failed");
         break;
      }
      memset(big, 'x', n);
      big[n] = '\0';

      char *argv[] = {(char *)"k", big};
      cJSON *req = cli_argspec_build("memory.store", spec, 2, argv);
      if (!req)
         fail("join", "refused a request it should have built");
      else
      {
         const cJSON *c = cJSON_GetObjectItemCaseSensitive(req, "content");
         if (!cJSON_IsString(c))
            fail("join", "content was not emitted as a string");
         else if (strlen(c->valuestring) != n)
            fail("join", "content was truncated or padded");
         cJSON_Delete(req);
      }
      free(big);
   }

   /* Multiple positionals still join with single spaces, across the old limit. */
   {
      char *word = malloc(5001);
      if (word)
      {
         memset(word, 'y', 5000);
         word[5000] = '\0';
         char *argv[] = {(char *)"k", word, word};
         cJSON *req = cli_argspec_build("memory.store", spec, 3, argv);
         const cJSON *c = req ? cJSON_GetObjectItemCaseSensitive(req, "content") : NULL;
         if (!cJSON_IsString(c) || strlen(c->valuestring) != 5000 + 1 + 5000)
            fail("join", "multi-word join lost bytes across the old 8 KiB limit");
         cJSON_Delete(req);
         free(word);
      }
   }
   cJSON_Delete(spec);
}

int main(void)
{
   /* The session source resolves --session, then $AIMEE_SESSION_ID, then the
    * literal "default". With the variable UNSET the first two are
    * indistinguishable, so inverting the precedence in the interpreter passed
    * unnoticed when it was planted. Set it to something no sample passes as a
    * flag, and the samples below separate the three steps. */
   setenv("AIMEE_SESSION_ID", "env-session-id", 1);
   /* The project source has the same fixed-context property: an explicit flag
    * wins, otherwise the launcher-provided project id is sent. A nonempty env
    * value makes every shared index-context sample prove the fallback rather
    * than accidentally agreeing when both sides omit it. */
   setenv("AIMEE_PROJECT_ID", "env-project-id", 1);

   test_every_shipped_spec_is_sampled();
   for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++)
      check_same(&SAMPLES[i]);
   test_refuses_what_it_cannot_build();
   test_positional_join_is_not_truncated();
   test_number_is_refused_not_coerced();

   if (g_fail == 0)
      printf("PASS test_cli_argspec (%zu shipped specs, %zu differential samples)\n",
             sizeof(SHIPPED) / sizeof(SHIPPED[0]), sizeof(SAMPLES) / sizeof(SAMPLES[0]));
   else
      printf("FAIL test_cli_argspec: %d check(s) failed\n", g_fail);
   return g_fail == 0 ? 0 : 1;
}
