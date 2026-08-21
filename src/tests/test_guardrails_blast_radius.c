/* test_guardrails_blast_radius.c: unit tests for the §7 structural blast-radius
 * advisory (proposal "code-graph intelligence" §7). Covers the pure formatter
 * (gate, listing, ellipsis, hub note, truncation-safety), the abs-path -> project
 * resolver, and the fail-open advisory gate. Hermetic: the config accessor + the two
 * kb_client_index_* calls are stubbed below, so no DB / sidecar is needed. */
#include "guardrails_blast_radius.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "kb_client.h"

/* ── Stubs: controllable KB sidecar + config ─────────────────────────────── */
static project_info_t g_projects[4];
static int g_project_count = 0;
static blast_radius_t g_blast;
static int g_blast_rc = 0;       /* what kb_client_index_blast_radius returns */
static int g_advisory_flag = 0;  /* what the config accessor reports for the §7 flag */
static char g_last_project[128]; /* project passed to the blast-radius fetch */
static char g_last_rel[256];     /* relpath passed to the blast-radius fetch */

int kb_client_index_list(project_info_t *out, int max)
{
   int n = g_project_count < max ? g_project_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_projects[i];
   return n;
}

int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   snprintf(g_last_project, sizeof(g_last_project), "%s", project ? project : "");
   snprintf(g_last_rel, sizeof(g_last_rel), "%s", file_path ? file_path : "");
   if (g_blast_rc != 0)
      return g_blast_rc;
   *out = g_blast;
   return 0;
}

/* The gate now asks config for one boolean instead of loading a whole legacy_config_record,
 * so the seam is the accessor. This is a smaller stub than the legacy_config_read it
 * replaces: a bool in, a bool out, with no need to know the struct's shape. */
int config_guardrails_blast_radius_advisory_enabled(void)
{
   return g_advisory_flag;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void set_blast(int count, const char *const *deps)
{
   memset(&g_blast, 0, sizeof(g_blast));
   g_blast.dependent_count = count;
   for (int i = 0; i < count && i < 64; i++)
      snprintf(g_blast.dependents[i], sizeof(g_blast.dependents[i]), "%s", deps[i]);
   g_blast_rc = 0;
}

/* ── Pure formatter ──────────────────────────────────────────────────────── */
static void test_format_no_dependents_is_silent(void)
{
   blast_radius_t br;
   memset(&br, 0, sizeof(br));
   char msg[256] = "";
   int rc =
       blast_radius_advisory_format(&br, "src/a.c", BR_ADVISORY_HUB_THRESHOLD, msg, sizeof(msg));
   assert(rc == 0);
   assert(msg[0] == '\0'); /* untouched */
   printf("  ok: no dependents -> silent\n");
}

static void test_format_lists_dependents(void)
{
   const char *deps[] = {"src/x.c", "src/y.c"};
   blast_radius_t br;
   memset(&br, 0, sizeof(br));
   br.dependent_count = 2;
   snprintf(br.dependents[0], sizeof(br.dependents[0]), "%s", deps[0]);
   snprintf(br.dependents[1], sizeof(br.dependents[1]), "%s", deps[1]);
   char msg[256] = "";
   int rc =
       blast_radius_advisory_format(&br, "src/a.c", BR_ADVISORY_HUB_THRESHOLD, msg, sizeof(msg));
   assert(rc == 1);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "src/a.c") != NULL); /* the edited file */
   assert(strstr(msg, "2 dependent files") != NULL);
   assert(strstr(msg, "src/x.c") != NULL && strstr(msg, "src/y.c") != NULL);
   assert(strstr(msg, "hub") == NULL); /* below hub threshold */
   printf("  ok: lists dependents, no hub note below threshold\n");
}

static void test_format_singular_count(void)
{
   const char *deps[] = {"src/only.c"};
   blast_radius_t br;
   memset(&br, 0, sizeof(br));
   br.dependent_count = 1;
   snprintf(br.dependents[0], sizeof(br.dependents[0]), "%s", deps[0]);
   char msg[256] = "";
   blast_radius_advisory_format(&br, "src/a.c", BR_ADVISORY_HUB_THRESHOLD, msg, sizeof(msg));
   assert(strstr(msg, "1 dependent file ") != NULL); /* singular, no trailing 's' */
   printf("  ok: singular 'dependent file'\n");
}

static void test_format_hub_note_and_ellipsis(void)
{
   /* 10 dependents: caps the list at BR_ADVISORY_MAX_NAMES, appends "(+N more)"
    * and the hub note (10 >= threshold). */
   char names[10][32];
   const char *deps[10];
   for (int i = 0; i < 10; i++)
   {
      snprintf(names[i], sizeof(names[i]), "src/dep%d.c", i);
      deps[i] = names[i];
   }
   blast_radius_t br;
   memset(&br, 0, sizeof(br));
   br.dependent_count = 10;
   for (int i = 0; i < 10; i++)
      snprintf(br.dependents[i], sizeof(br.dependents[i]), "%s", deps[i]);
   char msg[512] = "";
   int rc =
       blast_radius_advisory_format(&br, "src/core.c", BR_ADVISORY_HUB_THRESHOLD, msg, sizeof(msg));
   assert(rc == 1);
   assert(strstr(msg, "10 dependent files") != NULL);
   char more[32];
   snprintf(more, sizeof(more), "(+%d more)", 10 - BR_ADVISORY_MAX_NAMES);
   assert(strstr(msg, more) != NULL);
   assert(strstr(msg, "high-centrality hub") != NULL);
   /* the (BR_ADVISORY_MAX_NAMES+1)-th name must NOT appear in the listed names */
   assert(strstr(msg, "src/dep9.c") == NULL);
   printf("  ok: caps list, ellipsis, hub note at/above threshold\n");
}

static void test_format_truncation_safe(void)
{
   /* A tiny buffer must never overflow and still returns 1 (advisory warranted). */
   const char *deps[] = {"src/aaaaaaaaaaaaaaaaaaaa.c", "src/bbbbbbbbbbbbbbbbbbbb.c"};
   blast_radius_t br;
   memset(&br, 0, sizeof(br));
   br.dependent_count = 2;
   snprintf(br.dependents[0], sizeof(br.dependents[0]), "%s", deps[0]);
   snprintf(br.dependents[1], sizeof(br.dependents[1]), "%s", deps[1]);
   char msg[24] = "";
   int rc =
       blast_radius_advisory_format(&br, "src/a.c", BR_ADVISORY_HUB_THRESHOLD, msg, sizeof(msg));
   assert(rc == 1);
   assert(strlen(msg) < sizeof(msg)); /* NUL-terminated within bounds */
   printf("  ok: truncation-safe on a tiny buffer\n");
}

static void test_format_null_safe(void)
{
   char msg[64] = "";
   assert(blast_radius_advisory_format(NULL, "x", 5, msg, sizeof(msg)) == 0);
   blast_radius_t br;
   memset(&br, 0, sizeof(br));
   br.dependent_count = 1;
   snprintf(br.dependents[0], sizeof(br.dependents[0]), "x.c");
   assert(blast_radius_advisory_format(&br, "x", 5, NULL, 0) == 0);
   printf("  ok: null/zero-length args handled\n");
}

/* ── Resolver: abs path -> project + relpath ─────────────────────────────── */
static void test_resolve_matches_project(void)
{
   memset(g_projects, 0, sizeof(g_projects));
   snprintf(g_projects[0].name, sizeof(g_projects[0].name), "aimee");
   snprintf(g_projects[0].root, sizeof(g_projects[0].root), "/home/u/aimee");
   g_project_count = 1;
   const char *one[] = {"src/dep.c"};
   set_blast(1, one);

   blast_radius_t out;
   int rc = guardrails_blast_radius_for_abs_path("/home/u/aimee/src/file.c", &out);
   assert(rc == 0);
   assert(strcmp(g_last_project, "aimee") == 0);
   assert(strcmp(g_last_rel, "src/file.c") == 0); /* root stripped, no leading slash */
   assert(out.dependent_count == 1);
   printf("  ok: resolves project + strips root to relpath\n");
}

static void test_resolve_no_match_fails_open(void)
{
   memset(g_projects, 0, sizeof(g_projects));
   snprintf(g_projects[0].name, sizeof(g_projects[0].name), "aimee");
   snprintf(g_projects[0].root, sizeof(g_projects[0].root), "/home/u/aimee");
   g_project_count = 1;
   blast_radius_t out;
   /* Path under a different root, and a prefix that is NOT a path boundary. */
   assert(guardrails_blast_radius_for_abs_path("/home/u/other/x.c", &out) == -1);
   assert(guardrails_blast_radius_for_abs_path("/home/u/aimee-evil/x.c", &out) == -1);
   printf("  ok: no project match -> -1 (fail-open)\n");
}

static void test_resolve_sidecar_error_fails_open(void)
{
   memset(g_projects, 0, sizeof(g_projects));
   snprintf(g_projects[0].name, sizeof(g_projects[0].name), "aimee");
   snprintf(g_projects[0].root, sizeof(g_projects[0].root), "/home/u/aimee");
   g_project_count = 1;
   g_blast_rc = -1; /* sidecar failure */
   blast_radius_t out;
   assert(guardrails_blast_radius_for_abs_path("/home/u/aimee/a.c", &out) == -1);
   g_blast_rc = 0;
   printf("  ok: sidecar error -> -1 (fail-open)\n");
}

/* ── Advisory gate (config flag + msg_buf precedence) ────────────────────── */
static void test_advisory_disabled_is_silent(void)
{
   memset(g_projects, 0, sizeof(g_projects));
   snprintf(g_projects[0].name, sizeof(g_projects[0].name), "aimee");
   snprintf(g_projects[0].root, sizeof(g_projects[0].root), "/home/u/aimee");
   g_project_count = 1;
   const char *five[] = {"a.c", "b.c", "c.c", "d.c", "e.c"};
   set_blast(5, five);
   g_advisory_flag = 0; /* flag OFF */
   char msg[256] = "";
   guardrails_blast_radius_advisory("/home/u/aimee/src/file.c", msg, sizeof(msg));
   assert(msg[0] == '\0'); /* opt-in: nothing when disabled */
   printf("  ok: flag off -> no advisory\n");
}

static void test_advisory_enabled_emits(void)
{
   const char *five[] = {"a.c", "b.c", "c.c", "d.c", "e.c"};
   set_blast(5, five);
   g_advisory_flag = 1; /* flag ON */
   char msg[256] = "";
   guardrails_blast_radius_advisory("/home/u/aimee/src/file.c", msg, sizeof(msg));
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "high-centrality hub") != NULL); /* 5 >= hub threshold */
   printf("  ok: flag on + dependents -> advisory emitted\n");
}

static void test_advisory_respects_existing_message(void)
{
   const char *five[] = {"a.c", "b.c", "c.c", "d.c", "e.c"};
   set_blast(5, five);
   g_advisory_flag = 1;
   char msg[256];
   snprintf(msg, sizeof(msg), "BLOCKED: something higher priority");
   guardrails_blast_radius_advisory("/home/u/aimee/src/file.c", msg, sizeof(msg));
   assert(strcmp(msg, "BLOCKED: something higher priority") == 0); /* not clobbered */
   printf("  ok: non-empty msg_buf is not clobbered\n");
}

int main(void)
{
   printf("test_guardrails_blast_radius\n");
   test_format_no_dependents_is_silent();
   test_format_lists_dependents();
   test_format_singular_count();
   test_format_hub_note_and_ellipsis();
   test_format_truncation_safe();
   test_format_null_safe();
   test_resolve_matches_project();
   test_resolve_no_match_fails_open();
   test_resolve_sidecar_error_fails_open();
   test_advisory_disabled_is_silent();
   test_advisory_enabled_emits();
   test_advisory_respects_existing_message();
   printf("  all tests passed\n");
   return 0;
}
