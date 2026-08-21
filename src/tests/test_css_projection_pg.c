/* test_css_projection_pg.c: PROOF that a component and the stylesheet that styles
 * it meet in the unified code graph.
 *
 * This is the assertion the CSS work exists for. A stylesheet defines `.btn`, a
 * component names `btn` in its markup, and until now those two facts lived in
 * separate places: the class in a CSS-only table, the component in another, with
 * nothing in entity_edges joining them. Three things had to be true at once for
 * the join to close, and this asserts all three against a real database:
 *
 *   1. tree-sitter reads `.btn` out of the stylesheet as a DEFINITION, so the
 *      projection emits `file --defines--> symbol:proj:btn`.
 *   2. tree-sitter reads `btn` out of the component's JSX as a class token, which
 *      db2_css_component_resolve records.
 *   3. the projection emits that as `file --styles--> symbol:proj:btn`.
 *
 * Both edges land on the SAME node key, which is what makes the two files
 * reachable from one another. Asserting the edges separately would not show
 * that; asserting they share an endpoint does.
 *
 * REAL-PG ONLY: db2_code_projection_edge_upsert writes with to_char(CURRENT_
 * TIMESTAMP, ...), which the SQLite shim has no function for, so the projection
 * cannot run there at all. Reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0)
 * when it is unset, mirroring test_kb_audit_worm_pg.c, so `make unit-tests` on a
 * box without Postgres stays green. */
#include "modules/db2/c/code_index.h"
#include "modules/db2/c/code_projection.h"
#include "modules/db2/c/css_graph.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/entity_nodes.h"
#include "css_analyze.h"
#include "index.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROJECT "cssproj"

/* Leave no rows from a previous run: the assertions below count edges. */
static void reset(void *conn)
{
   char err[256] = "";
   aimee_pg_exec(conn,
                 "DELETE FROM entity_edges WHERE edge_origin = 'code_projection'"
                 " AND (source LIKE 'file:" PROJECT ":%' OR source LIKE 'project:" PROJECT ":%'"
                 "      OR source = 'project:" PROJECT "')",
                 err, sizeof err);
   aimee_pg_exec(conn, "DELETE FROM code_projection_generations WHERE project = '" PROJECT "'", err,
                 sizeof err);
   aimee_pg_exec(conn,
                 "DELETE FROM css_component_styles WHERE component_file_id IN"
                 " (SELECT f.id FROM files f JOIN projects p ON p.id = f.project_id"
                 "  WHERE p.name = '" PROJECT "')",
                 err, sizeof err);
   aimee_pg_exec(conn,
                 "DELETE FROM css_rules WHERE file_id IN"
                 " (SELECT f.id FROM files f JOIN projects p ON p.id = f.project_id"
                 "  WHERE p.name = '" PROJECT "')",
                 err, sizeof err);
}

/* Does `entity` carry an edge of `relation` whose target is `target`? */
static int has_edge(const char *entity, const char *relation, const char *target)
{
   edge_t edges[128];
   int n = db2_entity_edge_list_by_entity(entity, edges, 128);
   for (int i = 0; i < n; i++)
      if (strcmp(edges[i].relation, relation) == 0 && strcmp(edges[i].target, target) == 0)
         return 1;
   return 0;
}

static void report(const char *entity)
{
   edge_t edges[128];
   int n = db2_entity_edge_list_by_entity(entity, edges, 128);
   fprintf(stderr, "  %s has %d edge(s):\n", entity, n);
   for (int i = 0; i < n; i++)
      fprintf(stderr, "    -%s-> %s\n", edges[i].relation, edges[i].target);
}

static void run(void)
{
   void *conn = db2_conn();
   assert(conn);
   reset(conn);

   int64_t project_id = db2_code_index_project_upsert(PROJECT, "/" PROJECT);
   assert(project_id >= 0);

   /* The stylesheet defines .btn. */
   int64_t stylesheet_id =
       db2_code_index_file_upsert(project_id, "styles.css", "2026-01-01T00:00:00Z");
   assert(stylesheet_id >= 0);
   const char *css = ".btn { color: red }\n.unused { color: gray }\n";
   css_stylesheet_t *sheet = css_analyze(css, strlen(css));
   assert(sheet && sheet->rule_count == 2);
   assert(db2_css_graph_replace(stylesheet_id, sheet->rules, sheet->rule_count) == 0);
   css_stylesheet_free(sheet);

   /* The same call the indexer makes, which is where the class becomes a term
    * the projection can read. Before CSS surfaced class selectors as
    * definitions this returned nothing for a stylesheet. */
   definition_t definitions[64];
   int definition_count = extract_definitions(".css", css, definitions, 64);
   if (definition_count < 2)
   {
      fprintf(stderr, "css_projection_pg: the stylesheet yielded %d definition(s)\n",
              definition_count);
      for (int i = 0; i < definition_count; i++)
         fprintf(stderr, "    %s/%s\n", definitions[i].name, definitions[i].kind);
      assert(0);
   }
   {
      code_index_file_data_t data = {0};
      data.content = css;
      data.definitions = definitions;
      data.definition_count = definition_count;
      assert(db2_code_index_file_replace(stylesheet_id, &data) == 0);
   }

   /* The component names it. */
   int64_t component_id =
       db2_code_index_file_upsert(project_id, "Button.tsx", "2026-01-01T00:00:00Z");
   assert(component_id >= 0);
   const char *tsx = "export const Button = () => <button className=\"btn absent\">x</button>;\n";
   char tokens[64][CSS_CLASS_TOKEN_MAX];
   int token_count = css_extract_class_tokens(tsx, strlen(tsx), tokens, 64);
   assert(token_count == 2); /* btn, absent */
   assert(db2_css_component_resolve(component_id, tokens, token_count) == 0);

   /* Project and publish: a projected edge is visible only while its generation is. */
   int64_t generation = db2_code_projection_generation_create(PROJECT);
   assert(generation > 0);
   int64_t edges = db2_code_projection_sync_project(PROJECT, generation);
   if (edges <= 0)
   {
      fprintf(stderr, "css_projection_pg: the projection emitted %lld edges\n", (long long)edges);
      assert(0);
   }
   assert(db2_code_projection_generation_publish(generation, PROJECT) == 0);

   char stylesheet_key[GRAPH_ENDPOINT_MAX], component_key[GRAPH_ENDPOINT_MAX];
   char btn_key[GRAPH_ENDPOINT_MAX], absent_key[GRAPH_ENDPOINT_MAX];
   assert(db2_entity_node_key_file(PROJECT, "styles.css", stylesheet_key, sizeof(stylesheet_key)) ==
          0);
   assert(db2_entity_node_key_file(PROJECT, "Button.tsx", component_key, sizeof(component_key)) ==
          0);
   assert(db2_entity_node_key_symbol(PROJECT, "btn", btn_key, sizeof(btn_key)) == 0);
   assert(db2_entity_node_key_symbol(PROJECT, "absent", absent_key, sizeof(absent_key)) == 0);

   /* 1. The stylesheet defines the class, because tree-sitter reads a class
    *    selector as a definition rather than skipping selectors. */
   if (!has_edge(stylesheet_key, "defines", btn_key))
   {
      fprintf(stderr, "css_projection_pg: the stylesheet does not define %s\n", btn_key);
      report(stylesheet_key);
      assert(0);
   }

   /* 2. The component styles it, because the JSX walk read the class token and
    *    the projection emits css_component_styles as an edge. */
   if (!has_edge(component_key, "styles", btn_key))
   {
      fprintf(stderr, "css_projection_pg: the component does not style %s\n", btn_key);
      report(component_key);
      assert(0);
   }

   /* 3. A class the component names and no stylesheet defines is still an edge --
    *    a missing style is exactly what a reader wants to find, and dropping it
    *    would make it look like an absent reference instead. */
   if (!has_edge(component_key, "styles", absent_key))
   {
      fprintf(stderr, "css_projection_pg: the unresolved class %s is not an edge\n", absent_key);
      report(component_key);
      assert(0);
   }
   assert(!has_edge(stylesheet_key, "defines", absent_key));

   /* And the class nothing references is defined but unstyled, which is what
    *    makes dead-style analysis answerable from the graph. */
   char unused_key[GRAPH_ENDPOINT_MAX];
   assert(db2_entity_node_key_symbol(PROJECT, "unused", unused_key, sizeof(unused_key)) == 0);
   assert(has_edge(stylesheet_key, "defines", unused_key));
   assert(!has_edge(component_key, "styles", unused_key));

   printf("  stylesheet defines btn, component styles btn -- same node, two files: ok\n");
   printf("  unresolved class is an edge; unreferenced class is defined and unstyled: ok\n");
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("css_projection_pg: SKIP (AIMEE_TEST_PG_URL unset; real Postgres required)\n");
      return 0;
   }

   const char *tmpdir = getenv("TMPDIR");
   if (!tmpdir || !tmpdir[0])
      tmpdir = "/tmp";
   char home[256];
   snprintf(home, sizeof(home), "%s/aimee-css-projection-pg-%d", tmpdir, (int)getpid());
   char command[320];
   snprintf(command, sizeof(command), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(command) == 0);
   setenv("AIMEE_HOME", home, 1);

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "css_projection_pg: db2_init failed for %s\n", url);
      return 1;
   }

   printf("css_projection_pg:\n");
   run();
   db2_shutdown();
   printf("All css_projection_pg tests passed.\n");
   return 0;
}
