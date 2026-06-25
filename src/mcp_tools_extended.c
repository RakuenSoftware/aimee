/* mcp_tools_extended.c: P3 catalog extension.
 *
 * Read-only MCP tools that expose existing server/kb capabilities — project
 * roadmaps + task graph, code-index navigation, and memory match-explanation —
 * to external MCP clients. These were previously reachable only via /v1 or the
 * kb action surface. Definitions only; the matching content handlers live in
 * server_mcp_call_table.inc. Kept out of mcp_tools.c (which is at its line
 * budget), and added to the catalog via mcp_build_tools_list -> here. Because
 * the P2 default presentation profile is "core", these are not shown upfront but
 * are discoverable via find_tools/describe_tool and callable by name. */
#include "cJSON.h"
#include "headers/mcp_tools.h"
#include <stdio.h>
#include <string.h>

/* Append a {name, description, inputSchema:{type:object, properties:{}}} tool and
 * return it so the caller can attach properties / required entries. */
static cJSON *ext_tool(cJSON *tools, const char *name, const char *desc)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON *s = cJSON_CreateObject();
   cJSON_AddStringToObject(s, "type", "object");
   cJSON_AddObjectToObject(s, "properties");
   cJSON_AddItemToObject(t, "inputSchema", s);
   cJSON_AddItemToArray(tools, t);
   return t;
}

static void ext_prop(cJSON *tool, const char *key, const char *type, const char *desc)
{
   cJSON *s = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *props = cJSON_GetObjectItemCaseSensitive(s, "properties");
   cJSON *p = cJSON_AddObjectToObject(props, key);
   cJSON_AddStringToObject(p, "type", type);
   cJSON_AddStringToObject(p, "description", desc);
}

static void ext_require(cJSON *tool, const char *key)
{
   cJSON *s = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *req = cJSON_GetObjectItemCaseSensitive(s, "required");
   if (!req)
      req = cJSON_AddArrayToObject(s, "required");
   cJSON_AddItemToArray(req, cJSON_CreateString(key));
}

void mcp_add_extended_tools(cJSON *tools)
{
   if (!tools)
      return;
   cJSON *t;

   /* ── Planning: roadmaps + task graph ─────────────────────────────────────── */
   ext_tool(tools, "roadmap_list", "List the project's roadmaps (ids + summaries) as JSON.");

   t = ext_tool(tools, "roadmap_show",
                "Show one roadmap by id: its milestone / task tree, as JSON.");
   ext_prop(t, "roadmap_id", "string", "Roadmap id (see roadmap_list).");
   ext_require(t, "roadmap_id");

   t = ext_tool(tools, "task_list",
                "List tasks from the project task graph (id, parent, title, state, confidence).");
   ext_prop(t, "state", "string", "Filter by state (e.g. open, done). Omit for all states.");
   ext_prop(t, "session_id", "string", "Filter by originating session. Omit for all.");
   ext_prop(t, "limit", "integer", "Max tasks to return (default 100, max 500).");

   /* ── Code intelligence: index navigation ─────────────────────────────────── */
   t = ext_tool(tools, "index_find_callers",
                "Find call sites of a symbol across the indexed code: project, file, calling "
                "function, line.");
   ext_prop(t, "symbol", "string", "Symbol / function name to find callers of.");
   ext_prop(t, "project", "string", "Restrict to a project (optional; omit to search all).");
   ext_require(t, "symbol");

   t = ext_tool(tools, "index_structure",
                "List the definitions (functions / types) in an indexed file with line ranges.");
   ext_prop(t, "file_path", "string", "File path within the indexed project.");
   ext_prop(t, "project", "string", "Project the file belongs to (optional).");
   ext_require(t, "file_path");

   t = ext_tool(tools, "index_blast_radius",
                "Impact analysis for one file: the files that depend on it (dependents) and the "
                "files it depends on (dependencies), from the code index.");
   ext_prop(t, "file_path", "string", "File path within the indexed project.");
   ext_prop(t, "project", "string", "Project the file belongs to (optional).");
   ext_require(t, "file_path");

   /* ── Memory grounding: explain a retrieval + provenance/history ───────────── */
   t = ext_tool(tools, "memory_explain_match",
                "Explain WHY a memory matches a query: the per-signal score breakdown "
                "(lexical / semantic / entity / graph / cross-encoder / …).");
   ext_prop(t, "query", "string", "The query to score the memory against.");
   ext_prop(t, "memory_id", "integer", "Id of the memory to explain (e.g. from search_memory).");
   ext_require(t, "query");
   ext_require(t, "memory_id");

   t = ext_tool(tools, "memory_provenance",
                "Provenance trail of a memory: the recorded actions (create / update / supersede / "
                "…) with session + timestamp, for citing and trust-ranking.");
   ext_prop(t, "memory_id", "integer", "Id of the memory (e.g. from search_memory).");
   ext_require(t, "memory_id");

   t = ext_tool(tools, "memory_fact_history",
                "Version history of a fact by its key: prior + current entries, newest first.");
   ext_prop(t, "key", "string", "The fact key to fetch history for.");
   ext_require(t, "key");

   /* ── Observability ───────────────────────────────────────────────────────── */
   ext_tool(tools, "dashboard_metrics",
            "Operational snapshot: server metrics plus the vector-store status, as JSON.");

   /* ── Work queue ──────────────────────────────────────────────────────────── */
   t = ext_tool(tools, "work_list",
                "List items in the work queue (id, title, source, status, claim, result).");
   ext_prop(t, "status_filter", "string",
            "Filter by status: pending / claimed / done / failed / cancelled. Omit for all.");

   ext_tool(tools, "work_board",
            "The work queue grouped by status (pending / claimed / done / failed / cancelled).");

   /* ── Structured-PDF evidence: access-gated citation retrieval ─────────────── */
   t = ext_tool(
       tools, "pdf_search_chunks",
       "Search ingested PDF documents and return matching chunks with line-level citations "
       "(page_no, bbox, quote). The entry point for PDF evidence; escalate a hit with "
       "pdf_open_page / pdf_open_neighbors / pdf_inspect_structure. Withholds quarantined docs.");
   ext_prop(t, "query", "string", "Text to search for across PDF chunk content.");
   ext_prop(t, "project", "string",
            "Project to search within (required; scopes the search to your access).");
   ext_prop(t, "max_results", "integer",
            "Max chunks to return (default 10; the route caps at 10).");
   ext_require(t, "query");
   ext_require(t, "project");

   t = ext_tool(tools, "pdf_open_page",
                "All citations on one page of a PDF document (page_no, bbox, quote per region).");
   ext_prop(t, "project", "string", "Project the document belongs to.");
   ext_prop(t, "document_key", "string", "Document key from a pdf_search_chunks hit.");
   ext_prop(t, "page_no", "integer", "1-based page number to open.");
   ext_require(t, "project");
   ext_require(t, "document_key");
   ext_require(t, "page_no");

   t = ext_tool(tools, "pdf_open_neighbors",
                "The chunks immediately before/after a PDF chunk, for surrounding context.");
   ext_prop(t, "project", "string", "Project the chunk belongs to (required; scopes the lookup).");
   ext_prop(t, "chunk_id", "integer", "chunk_id from a pdf_search_chunks hit.");
   ext_require(t, "project");
   ext_require(t, "chunk_id");

   t = ext_tool(tools, "pdf_inspect_structure",
                "The chunk outline of a PDF document (chunk_index, page range, heading path).");
   ext_prop(t, "project", "string", "Project the document belongs to.");
   ext_prop(t, "document_key", "string", "Document key from a pdf_search_chunks hit.");
   ext_require(t, "project");
   ext_require(t, "document_key");
}

/* ── Tool-family multiplexing (P4) ────────────────────────────────────────────
 * Several coherent families (a noun with verb operations) are presented as ONE
 * tool with a discriminator property whose value selects the operation, instead
 * of N separate tools. At build time mcp_collapse_families() folds each family's
 * member tools into one (merging their schemas); at dispatch time
 * mcp_family_demux() rewrites <family>({command|action:"verb"}) to the legacy
 * <family>_<verb> name so the existing handlers + capability gating run
 * unchanged. The legacy names stay directly callable. Discriminator is "command"
 * except where a member already owns a "command" param (background → "action"). */
struct fam_member
{
   const char *command;
   const char *tool;
};
struct fam_def
{
   const char *name;
   const char *cmd_key;
   const char *description;
   struct fam_member members[12];
};
static const struct fam_def MCP_FAMILIES[] = {
    {"pipeline",
     "command",
     "Roundtable authoring pipeline. Set 'command' to the operation; other params apply per "
     "command (see describe_tool).",
     {{"start", "pipeline_start"},
      {"advance", "pipeline_advance"},
      {"status", "pipeline_status"},
      {"list", "pipeline_list"},
      {"gate", "pipeline_gate"},
      {"resume", "pipeline_resume"},
      {"cancel", "pipeline_cancel"},
      {NULL, NULL}}},
    {"diagnose",
     "command",
     "Structured diagnosis session (observe → hypothesize → weigh evidence). Set 'command'.",
     {{"start", "diagnose_start"},
      {"observe", "diagnose_observe"},
      {"hypothesize", "diagnose_hypothesize"},
      {"evidence", "diagnose_evidence"},
      {"status", "diagnose_status"},
      {NULL, NULL}}},
    {"session",
     "command",
     "Session operations: workflow sessions (start/status/pause/advance/list), conversation "
     "transcript search (transcript_search), and current-session virtual-context stubs "
     "(context_search/context_expand/context_status). Set 'command'.",
     {{"start", "session_start"},
      {"status", "session_status"},
      {"pause", "session_pause"},
      {"advance", "session_advance"},
      {"list", "session_list"},
      {"transcript_search", "session_search"},
      {"context_search", "session_context_search"},
      {"context_expand", "session_context_expand"},
      {"context_status", "session_context_status"},
      {NULL, NULL}}},
    {"lsp",
     "command",
     "Language-server queries over the workspace. Set 'command'.",
     {{"diagnostics", "lsp_diagnostics"},
      {"definition", "lsp_definition"},
      {"references", "lsp_references"},
      {NULL, NULL}}},
    {"index",
     "command",
     "Code-index navigation. Set 'command'.",
     {{"find_callers", "index_find_callers"},
      {"structure", "index_structure"},
      {"blast_radius", "index_blast_radius"},
      {"preview", "preview_blast_radius"},
      {NULL, NULL}}},
    {"note",
     "command",
     "Investigation notes. Set 'command'.",
     {{"create", "create_note"}, {"list", "list_notes"}, {"search", "search_notes"}, {NULL, NULL}}},
    {"prospective_memory",
     "command",
     "'When X, surface Y' reminders. Set 'command'.",
     {{"create", "create_prospective_memory"},
      {"list", "list_prospective_memories"},
      {"complete", "complete_prospective_memory"},
      {NULL, NULL}}},
    {"epistemic_directive",
     "command",
     "Open 'ask the user' directives. Set 'command'.",
     {{"create", "create_epistemic_directive"},
      {"list", "list_epistemic_directives"},
      {"resolve", "resolve_epistemic_directive"},
      {NULL, NULL}}},
    {"background",
     "action",
     "Background shell processes. Set 'action'; run also takes 'command' (the shell command).",
     {{"run", "run_background_process"},
      {"output", "get_background_output"},
      {"kill", "kill_background_process"},
      {"list", "list_background_processes"},
      {NULL, NULL}}},
    /* ── P4b: knowledge/memory cluster split into coherent sub-families ──────── */
    {"graph",
     "command",
     "Knowledge-graph & entity exploration. Set 'command'.",
     {{"search", "search_graph"},
      {"episode", "get_episode"},
      {"entity", "get_entity"},
      {"entity_edges", "get_entity_edges"},
      {NULL, NULL}}},
    {"memory",
     "command",
     "Operate on stored memories (the per-record lifecycle + introspection). Use search_memory / "
     "memory_recall for retrieval. Set 'command'.",
     {{"get", "memory_get"},
      {"list", "list_facts"},
      {"mutate", "mutate"},
      {"history", "memory_fact_history"},
      {"provenance", "memory_provenance"},
      {"explain", "memory_explain_match"},
      {"maintain", "memory_maintain"},
      {NULL, NULL}}},
    {"recall",
     "command",
     "Proactive memory bundles for the current task/session. Set 'command'.",
     {{"context_block", "get_context_block"},
      {"briefing", "memory_briefing"},
      {"alerts", "memory_alerts"},
      {NULL, NULL}}},
    /* ── P4b: clean two-tool families ───────────────────────────────────────── */
    {"clarify",
     "command",
     "Clarification session. Set 'command'.",
     {{"start", "clarify_start"}, {"answer", "clarify_answer"}, {NULL, NULL}}},
    {"learning",
     "command",
     "Learning signals. Set 'command'.",
     {{"propose", "learning_propose"}, {"review", "learning_review"}, {NULL, NULL}}},
    {"rules",
     "command",
     "Collaborative rules. Set 'command'.",
     {{"propose", "rules_propose"}, {"list", "rules_list"}, {NULL, NULL}}},
    {"roadmap",
     "command",
     "Project roadmaps. Set 'command'.",
     {{"list", "roadmap_list"}, {"show", "roadmap_show"}, {NULL, NULL}}},
    {"work",
     "command",
     "Work queue. Set 'command'.",
     {{"list", "work_list"}, {"board", "work_board"}, {NULL, NULL}}},
    {"job",
     "command",
     "Coordinated parallel jobs. Set 'command'.",
     {{"start", "job_start"}, {"status", "job_status"}, {NULL, NULL}}},
    {"host",
     "command",
     "Infrastructure host inventory. Set 'command'.",
     {{"get", "get_host"}, {"list", "list_hosts"}, {NULL, NULL}}},
    {"attempt",
     "command",
     "Record / review failed approaches. Set 'command'.",
     {{"record", "record_attempt"}, {"list", "list_attempts"}, {NULL, NULL}}},
    {NULL, NULL, NULL, {{NULL, NULL}}},
};

static cJSON *family_detach_member(cJSON *tools, const char *name)
{
   int n = cJSON_GetArraySize(tools);
   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_GetArrayItem(tools, i);
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
      if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0)
         return cJSON_DetachItemFromArray(tools, i);
   }
   return NULL;
}

void mcp_collapse_families(cJSON *tools)
{
   if (!tools || !cJSON_IsArray(tools))
      return;
   for (const struct fam_def *f = MCP_FAMILIES; f->name; f++)
   {
      cJSON *ft = cJSON_CreateObject();
      cJSON_AddStringToObject(ft, "name", f->name);
      cJSON_AddStringToObject(ft, "description", f->description);
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_AddObjectToObject(s, "properties");
      cJSON *disc = cJSON_AddObjectToObject(props, f->cmd_key);
      cJSON_AddStringToObject(disc, "type", "string");
      cJSON *en = cJSON_AddArrayToObject(disc, "enum");

      char clist[256] = "";
      size_t cl = 0;
      int found = 0;
      for (const struct fam_member *m = f->members; m->command; m++)
      {
         cJSON_AddItemToArray(en, cJSON_CreateString(m->command));
         if (cl < sizeof(clist))
            cl += (size_t)snprintf(clist + cl, sizeof(clist) - cl, "%s%s", cl ? ", " : "",
                                   m->command);
         cJSON *mt = family_detach_member(tools, m->tool);
         if (!mt)
            continue;
         found++;
         cJSON *ms = cJSON_GetObjectItemCaseSensitive(mt, "inputSchema");
         cJSON *mp = ms ? cJSON_GetObjectItemCaseSensitive(ms, "properties") : NULL;
         cJSON *pr = NULL;
         cJSON_ArrayForEach(pr, mp)
         {
            if (!pr->string || strcmp(pr->string, f->cmd_key) == 0)
               continue;
            if (!cJSON_GetObjectItemCaseSensitive(props, pr->string))
               cJSON_AddItemToObject(props, pr->string, cJSON_Duplicate(pr, 1));
         }
         cJSON_Delete(mt);
      }
      char dbuf[320];
      snprintf(dbuf, sizeof(dbuf), "Operation to run (one of: %s).", clist);
      cJSON_AddStringToObject(disc, "description", dbuf);
      cJSON *req = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(req, cJSON_CreateString(f->cmd_key));
      cJSON_AddItemToObject(ft, "inputSchema", s);
      if (found)
         cJSON_AddItemToArray(tools, ft);
      else
         cJSON_Delete(ft); /* members absent (e.g. partial build) — leave list as-is */
   }
}

int mcp_family_demux(const char *tool, cJSON *args, char *out, size_t n)
{
   for (const struct fam_def *f = MCP_FAMILIES; f->name; f++)
   {
      if (strcmp(tool, f->name) != 0)
         continue;
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(args, f->cmd_key);
      if (!cJSON_IsString(jc) || !jc->valuestring[0])
         return -1;
      for (const struct fam_member *m = f->members; m->command; m++)
         if (strcmp(jc->valuestring, m->command) == 0)
         {
            snprintf(out, n, "%s", m->tool);
            return 1;
         }
      return -1;
   }
   return 0;
}
