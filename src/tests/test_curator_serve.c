/* test_curator_serve.c: read-side curator serving over the sqlite shim — seeds
 * the artifact graph and asserts the JSON for /v1/implements, /v1/synthesize and
 * /v1/contradictions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../kb_curator_serve.h"

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_implements(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO projects (id,name,root,scanned_at,current_generation)"
            " VALUES (1,'p','/p','t',2)");
   seed(db, "INSERT INTO artifacts (id,kind,state,payload) VALUES"
            " ('e1','entity','committed','{\"name\":\"pgvector\"}'),"
            " ('cu-old','code_unit','committed',"
            "  '{\"summary\":\"stale vector store\",\"generation\":1}'),"
            " ('cu','code_unit','committed',"
            "  '{\"summary\":\"vector store\",\"generation\":2}')");
   seed(db, "UPDATE artifacts SET scope_kind='project',scope_id='p'"
            " WHERE id IN ('cu-old','cu')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES"
            " ('cu-old','e1','mentions'),('cu','e1','mentions')");
   seed(db, "INSERT INTO artifact_citations (artifact_id,source_kind,source_id)"
            " VALUES ('cu-old','kb_file','src/old.c'),('cu','kb_file','src/v.c')");

   char buf[4096];
   int n = kb_curator_implements_json("pgvector", buf, sizeof(buf));
   assert(n == 1);
   assert(strstr(buf, "\"cu\"") && strstr(buf, "src/v.c") && strstr(buf, "vector store"));
   assert(strstr(buf, "cu-old") == NULL && strstr(buf, "src/old.c") == NULL);
   /* unknown topic → empty list, count 0 */
   assert(kb_curator_implements_json("nope", buf, sizeof(buf)) == 0);
   db2_test_shim_close();
   printf("  /v1/implements traverses mentions -> code_units OK\n");
}

static void test_synthesize(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,payload) VALUES"
            " ('e1','entity','committed','{\"name\":\"pgvector\"}'),"
            " ('syn','synthesis','committed',"
            "'{\"text\":\"pgvector backs search\",\"citations\":[\"100\",\"200\"]}')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES ('syn','e1','about')");

   char buf[4096];
   int n = kb_curator_synthesize_serve_json("pgvector", buf, sizeof(buf));
   assert(n == 2);
   assert(strstr(buf, "pgvector backs search") && strstr(buf, "100") && strstr(buf, "200"));
   db2_test_shim_close();
   printf("  /v1/synthesize returns the cited narrative OK\n");
}

static void test_contradictions(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,payload) VALUES"
            " ('c1','claim','committed','{\"text\":\"auth uses JWT\"}'),"
            " ('c2','claim','committed','{\"text\":\"auth uses opaque tokens\"}')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES ('c1','c2','contradicts')");

   char buf[4096];
   int n = kb_curator_contradictions_json(10, buf, sizeof(buf));
   assert(n == 1);
   assert(strstr(buf, "auth uses JWT") && strstr(buf, "auth uses opaque tokens"));
   db2_test_shim_close();
   printf("  /v1/contradictions lists claim pairs OK\n");
}

int main(void)
{
   test_implements();
   test_synthesize();
   test_contradictions();
   printf("curator_serve: all tests passed\n");
   return 0;
}
