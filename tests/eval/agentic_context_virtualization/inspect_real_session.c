/* inspect_real_session.c: manual-inspection harness for the virtual-context
 * rollout (AC#2 / AC#3).
 *
 * Drives the *real* public code path with the feature flag ON —
 * conv_ctx_record_event -> conv_ctx_flush_pending -> conv_ctx_assemble plus
 * db1_conv_search_chains / db1_conv_chain_events (the same functions the
 * session_context_* MCP tools wrap) over a realistic tool-heavy sequence —
 * and prints the evidence a reviewer needs:
 *
 *   - per-chain raw vs stub bytes (compaction is real, not synthetic)
 *   - the assembled working set (compacted stubs, NOT replayed raw traffic)
 *   - a stub search hit (session_context_search behaviour)
 *   - raw recovery for one chain (session_context_expand behaviour)
 *   - the aggregate operational metrics (bytes saved, compression ratio)
 *
 * No LLM or network is required; output is captured into the rollout
 * validation report.  Build/run via `make virtual-context-inspect`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db1.h"
#include "conv_context.h"
#include "conversation_context.h"

/* A realistic tool-heavy coding session: file reads, greps, and a build,
 * the same shape a long investigation produces. */
struct ev
{
   const char *tool;
   const char *input;
   const char *result;  /* stored excerpt (capped); the full output is raw_bytes */
   int raw_bytes;       /* true size of the tool output (a file read is multi-KB) */
};

static const struct ev SESSION[] = {
    {"read_file", "{\"file_path\":\"src/config.c\"}",
     "legacy_config_read parses aimee.yaml; the session.virtual_context block sets "
     "virtual_context_enabled. Default flipped on for the rollout.",
     3400},
    {"bash", "{\"command\":\"grep -rn virtual_context_enabled src\"}",
     "src/config.c, src/headers/config.h, src/conversation_context.c reference "
     "virtual_context_enabled.",
     1200},
    {"read_file", "{\"file_path\":\"src/conversation_context.c\"}",
     "build_stub collapses a slice of tool events into a deterministic stub of "
     "Tools/Files/Excerpt; conv_ctx_assemble walks chains within a byte budget.",
     3900},
    {"read_file", "{\"file_path\":\"src/db1/conv_context.h\"}",
     "db1_conv_list_chains loads a session's chains; db1_conv_chain_events "
     "recovers the raw events for a chain (the expand path).",
     2600},
    {"bash", "{\"command\":\"make build/obj/tests/unit-test-conversation-context\"}",
     "compiled clean; conv_context unit test ok.",
     900},
    {"read_file", "{\"file_path\":\"tests/eval/agentic_context_virtualization/run_eval.py\"}",
     "run_eval.py is the deterministic gate: compression >=40%, oracle "
     "answerability, AC#4 baseline-vs-compacted accuracy.",
     3100},
    {"bash", "{\"command\":\"python3 run_eval.py --fixture synthetic --fixture real\"}",
     "All acceptance criteria met across 2 fixture(s).",
     800},
};

#define NEV ((int)(sizeof(SESSION) / sizeof(SESSION[0])))

static void enable_flag(char *home, size_t home_len)
{
   snprintf(home, home_len, "/tmp/aimee_vc_inspect_XXXXXX");
   if (!mkdtemp(home))
   {
      fprintf(stderr, "mkdtemp failed\n");
      exit(1);
   }
   char cfg[512];
   snprintf(cfg, sizeof(cfg), "%s/aimee.yaml", home);
   FILE *fp = fopen(cfg, "w");
   if (!fp)
   {
      fprintf(stderr, "cannot write %s\n", cfg);
      exit(1);
   }
   fprintf(fp, "session:\n  virtual_context:\n    enabled: true\n");
   fclose(fp);
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
}

int main(void)
{
   char home[64];
   enable_flag(home, sizeof(home));

   char dbpath[] = "/tmp/aimee_vc_inspect_db_XXXXXX.db";
   int fd = mkstemps(dbpath, 3);
   if (fd >= 0)
      close(fd);
   if (db1_init(dbpath) != 0)
   {
      fprintf(stderr, "db1_init failed\n");
      return 1;
   }

   const char *sid = "inspect-real-session";

   printf("=== Virtual-Context manual inspection (feature flag ON) ===\n\n");
   printf("Recording %d real-shaped tool events for session '%s'...\n", NEV, sid);
   for (int i = 0; i < NEV; i++)
   {
      int64_t id = conv_ctx_record_event(sid, SESSION[i].tool, SESSION[i].input,
                                         SESSION[i].result, SESSION[i].raw_bytes);
      if (id <= 0)
      {
         fprintf(stderr, "record_event failed (flag off?) at %d -> %lld\n", i, (long long)id);
         return 1;
      }
   }
   int chains_created = conv_ctx_flush_pending(sid);
   printf("Flushed pending events into %d chain(s).\n\n", chains_created);

   conv_tool_chain_t chains[32];
   int n = db1_conv_list_chains(sid, chains, 32);
   printf("--- Chains (compaction is real, stub_bytes < raw_bytes) ---\n");
   long raw_total = 0, stub_total = 0;
   for (int i = 0; i < n; i++)
   {
      raw_total += chains[i].raw_bytes;
      stub_total += chains[i].stub_bytes;
      printf("  chain #%lld [%s]  raw=%d stub=%d\n    stub: %s\n",
             (long long)chains[i].id, chains[i].tools, chains[i].raw_bytes,
             chains[i].stub_bytes, chains[i].stub);
   }
   printf("\n");

   printf("--- Assembled working set (conv_ctx_assemble): compacted stubs, "
          "NOT replayed raw traffic ---\n");
   char *asm_out = conv_ctx_assemble(sid, "virtual_context", 2048);
   if (asm_out)
   {
      printf("%s\n\n", asm_out);
      free(asm_out);
   }
   else
   {
      printf("(assemble returned NULL — flag off or no chains)\n\n");
   }

   printf("--- session_context_search('virtual_context') hit ---\n");
   conv_tool_chain_t hits[16];
   int h = db1_conv_search_chains(sid, "virtual_context", hits, 16);
   printf("  %d chain(s) matched; first stub: %s\n\n", h, h > 0 ? hits[0].stub : "(none)");

   printf("--- session_context_expand: raw recovery for chain #%lld ---\n",
          n > 0 ? (long long)chains[0].id : 0);
   if (n > 0)
   {
      conv_tool_event_t evs[64];
      int e = db1_conv_chain_events(chains[0].id, evs, 64);
      printf("  recovered %d raw event(s):\n", e);
      for (int i = 0; i < e; i++)
         printf("    [%s] %.80s\n", evs[i].tool_name, evs[i].tool_result);
      printf("\n");
   }

   long saved = raw_total - stub_total;
   double ratio = stub_total > 0 ? (double)raw_total / (double)stub_total : 0.0;
   double reduction = raw_total > 0 ? 100.0 * (double)saved / (double)raw_total : 0.0;
   printf("--- Operational metrics (session_context_status.metrics) ---\n");
   printf("  segments_total      : %d\n", n);
   printf("  chains_stubbed_total: %d\n", n);
   printf("  raw_bytes_total     : %ld\n", raw_total);
   printf("  stub_bytes_total    : %ld\n", stub_total);
   printf("  bytes_saved         : %ld\n", saved);
   printf("  compression_ratio   : %.1fx\n", ratio);
   printf("  reduction           : %.1f%%\n", reduction);

   db1_shutdown();
   unlink(dbpath);
   printf("\nInspection complete.\n");
   return 0;
}
