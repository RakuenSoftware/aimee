/* `aimee workspace add --no-scan` must register without walking the tree.
 *
 * workspace.add registers the workspace and then, when kb is live, eagerly scans
 * every discovered project before answering. On a 4,000-file repository that
 * turns a registration RPC into a multi-minute one: a caller with a timeout
 * abandons a registration that had already succeeded, and the projects it was
 * waiting on get indexed by the background ingest timer regardless.
 *
 * Observed on a benchmark host with load ~4 (i.e. not contention): three cells
 * lost to `aimee workspace add` exceeding a 180s client timeout.
 *
 * The flag has to reach the server as a request field, so this pins the
 * marshalling: --no-scan sets scan:false, and its absence sends nothing at all
 * (so an older server, which ignores the unknown field, keeps today's
 * behaviour). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* The marshaller is static; the suite's convention for those is to include the
 * translation unit (see test_anthropic_http.c and friends). */
#include "../cli_v1_routes_b.c"

static cJSON *marshal(int argc, char **argv)
{
   cJSON *req = marshal_workspace_add(argc, argv);
   assert(req != NULL);
   return req;
}

static void test_no_scan_sets_scan_false(void)
{
   char *argv[] = {"/srv/repo", "--no-scan"};
   cJSON *req = marshal(2, argv);

   const cJSON *scan = cJSON_GetObjectItemCaseSensitive(req, "scan");
   assert(cJSON_IsBool(scan));
   assert(cJSON_IsFalse(scan));

   /* The root must still be carried: skipping the scan is not skipping the
    * registration. */
   const cJSON *root = cJSON_GetObjectItemCaseSensitive(req, "root");
   assert(cJSON_IsString(root));
   assert(strcmp(root->valuestring, "/srv/repo") == 0);

   cJSON_Delete(req);
   printf("  --no-scan sets scan:false and keeps root\n");
}

static void test_default_sends_no_scan_field(void)
{
   char *argv[] = {"/srv/repo"};
   cJSON *req = marshal(1, argv);

   /* Absent, not `true`: an older server that does not know the field must see
    * exactly the body it saw before, and keep scanning eagerly. */
   assert(cJSON_GetObjectItemCaseSensitive(req, "scan") == NULL);

   const cJSON *root = cJSON_GetObjectItemCaseSensitive(req, "root");
   assert(cJSON_IsString(root));
   assert(strcmp(root->valuestring, "/srv/repo") == 0);

   cJSON_Delete(req);
   printf("  default omits the field (unchanged wire body)\n");
}

static void test_no_scan_composes_with_provider(void)
{
   char *argv[] = {"/srv/repo", "--provider", "git", "--no-scan"};
   cJSON *req = marshal(4, argv);

   const cJSON *scan = cJSON_GetObjectItemCaseSensitive(req, "scan");
   assert(cJSON_IsBool(scan) && cJSON_IsFalse(scan));
   const cJSON *prov = cJSON_GetObjectItemCaseSensitive(req, "provider");
   assert(cJSON_IsString(prov));
   assert(strcmp(prov->valuestring, "git") == 0);

   cJSON_Delete(req);
   printf("  --no-scan composes with --provider\n");
}

/* A `mirror` registration is refused without --remote, so the flags have to
 * reach the server as TOP-LEVEL fields: this body is delivered through POST
 * /v1/workspaces (including locally, over the HTTP UDS), and that route reads
 * the body, not `args`. Left in args alone they were dropped in transit, and
 * `aimee workspace add <path> --provider mirror --remote <url>` was answered
 * with "--provider mirror requires --remote <url>" — the server refusing over a
 * flag the caller had supplied. */
static void test_mirror_coordinates_are_surfaced(void)
{
   char *argv[] = {"/srv/repo", "--provider", "mirror", "--remote", "git@github.com:o/r.git",
                   "--head",    "abc123"};
   cJSON *req = marshal(7, argv);

   const cJSON *prov = cJSON_GetObjectItemCaseSensitive(req, "provider");
   assert(cJSON_IsString(prov) && strcmp(prov->valuestring, "mirror") == 0);
   const cJSON *rem = cJSON_GetObjectItemCaseSensitive(req, "remote");
   assert(cJSON_IsString(rem) && strcmp(rem->valuestring, "git@github.com:o/r.git") == 0);
   const cJSON *head = cJSON_GetObjectItemCaseSensitive(req, "head");
   assert(cJSON_IsString(head) && strcmp(head->valuestring, "abc123") == 0);

   cJSON_Delete(req);
   printf("  --remote/--head are surfaced for a mirror registration\n");
}

/* Absent flags send nothing, so a shared/detached registration keeps exactly the
 * body it had before and an older server sees no unknown fields. */
static void test_absent_coordinates_send_nothing(void)
{
   char *argv[] = {"/srv/repo", "--provider", "detached"};
   cJSON *req = marshal(3, argv);

   assert(cJSON_GetObjectItemCaseSensitive(req, "remote") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(req, "head") == NULL);

   cJSON_Delete(req);
   printf("  absent --remote/--head send nothing\n");
}

int main(void)
{
   printf("workspace add --no-scan:\n");
   test_no_scan_sets_scan_false();
   test_default_sends_no_scan_field();
   test_no_scan_composes_with_provider();
   test_mirror_coordinates_are_surfaced();
   test_absent_coordinates_send_nothing();
   printf("workspace add --no-scan: OK\n");
   return 0;
}
