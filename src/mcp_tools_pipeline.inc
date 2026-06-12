/* mcp_tools_pipeline.inc: roundtable authoring pipeline (pipeline_*) MCP tool
 * definitions, #included by mcp_build_tools_list() in mcp_tools.c (kept as an
 * .inc to stay under the per-file line cap). */

   /* Roundtable authoring pipeline (pipeline_* tools): idea -> reviewed proposal
    * -> implementation -> reviewed PR, with two human gates and two roundtable
    * quality gates. The driving agent may run the loop but CANNOT resolve a gate
    * (that needs an operator principal). */
   {
      cJSON_AddItemToArray(
          tools,
          build_tool(
              "pipeline_start",
              "Start a roundtable authoring pipeline from a one-line idea. Creates a dedicated "
              "proposal branch/worktree in repo_root and enters drafting. Returns the pipeline_id.",
              cJSON_Parse(
                  "{\"type\":\"object\",\"properties\":{"
                  "\"idea\":{\"type\":\"string\",\"description\":\"One-line idea/goal for the "
                  "proposal.\"},"
                  "\"repo_root\":{\"type\":\"string\",\"description\":\"Repository root for the "
                  "dedicated proposal/impl branch+worktree (required).\"},"
                  "\"base_branch\":{\"type\":\"string\",\"description\":\"PR base branch (default "
                  "testing).\"},"
                  "\"head_branch\":{\"type\":\"string\",\"description\":\"Optional dedicated head "
                  "branch; defaults to roundtable/proposal-<id>.\"},"
                  "\"remote\":{\"type\":\"string\",\"description\":\"Git remote (default "
                  "origin).\"},"
                  "\"done_bar\":{\"type\":\"string\",\"enum\":[\"zero_blocking\",\"zero_blocking_"
                  "suggestions\",\"zero_blocking_questions_answered\"],\"description\":\"Review "
                  "done-bar (default from config).\"},"
                  "\"brief\":{\"type\":\"string\",\"description\":\"Optional seed brief/focus.\"},"
                  "\"questions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
                  "\"description\":\"Optional accepted questions (max 16) for the "
                  "questions-answered bar.\"}},"
                  "\"required\":[\"idea\",\"repo_root\"]}")));

      cJSON_AddItemToArray(
          tools,
          build_tool(
              "pipeline_advance",
              "Drive one tick of the pipeline loop: submit a DRAFT/REVIEW roundtable pass, decide "
              "the captured pass (pass/revise/retry/escalate), open a PR + surface a gate when the "
              "done-bar is met, or reconcile a pending merge. Returns the next action.",
              cJSON_Parse(
                  "{\"type\":\"object\",\"properties\":{"
                  "\"pipeline_id\":{\"type\":\"integer\",\"description\":\"Pipeline id.\"},"
                  "\"artifact\":{\"type\":\"string\",\"description\":\"Optional revised proposal "
                  "text / diff to review; if omitted the controller reads the working proposal or "
                  "captures the diff itself.\"}},"
                  "\"required\":[\"pipeline_id\"]}")));

      cJSON_AddItemToArray(
          tools,
          build_tool("pipeline_status",
                     "Show a pipeline's state, phase, latest review digest, gate, panel diversity, "
                     "and economics.",
                     cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                                 "\"pipeline_id\":{\"type\":\"integer\",\"description\":\"Pipeline "
                                 "id.\"}},\"required\":[\"pipeline_id\"]}")));

      cJSON_AddItemToArray(
          tools, build_tool("pipeline_list", "List roundtable authoring pipelines.",
                            cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                                        "\"state\":{\"type\":\"string\",\"description\":\"Optional "
                                        "state filter; omit for all non-terminal.\"}}}")));

      cJSON_AddItemToArray(
          tools,
          build_tool(
              "pipeline_gate",
              "Resolve a human gate (pass|fail). REQUIRES an enrolled local operator principal — a "
              "delegate-driving session cannot pass its own gate. pass merges the PR (drift-safe) "
              "and advances; fail returns the reason to the brief and re-enters review.",
              cJSON_Parse(
                  "{\"type\":\"object\",\"properties\":{"
                  "\"pipeline_id\":{\"type\":\"integer\",\"description\":\"Pipeline id.\"},"
                  "\"verdict\":{\"type\":\"string\",\"enum\":[\"pass\",\"fail\"],\"description\":"
                  "\"Gate verdict.\"},"
                  "\"reason\":{\"type\":\"string\",\"description\":\"Fail reason (folded into the "
                  "next review brief).\"},"
                  "\"operator_principal\":{\"type\":\"string\",\"description\":\"Enrolled local "
                  "operator principal authorizing the gate.\"},"
                  "\"admin\":{\"type\":\"boolean\",\"description\":\"Use gh pr merge --admin.\"}},"
                  "\"required\":[\"pipeline_id\",\"verdict\",\"operator_principal\"]}")));

      cJSON_AddItemToArray(
          tools,
          build_tool(
              "pipeline_resume",
              "Resume a pipeline from the durable ledger across sessions/restarts; optionally "
              "repair repo/workspace metadata (repo_root/remote/head_branch/worktree_path) after "
              "an implementation-workspace failure.",
              cJSON_Parse(
                  "{\"type\":\"object\",\"properties\":{"
                  "\"pipeline_id\":{\"type\":\"integer\",\"description\":\"Pipeline id.\"},"
                  "\"repo_root\":{\"type\":\"string\"},\"remote\":{\"type\":\"string\"},"
                  "\"head_branch\":{\"type\":\"string\"},\"worktree_path\":{\"type\":\"string\"}},"
                  "\"required\":[\"pipeline_id\"]}")));

      cJSON_AddItemToArray(
          tools,
          build_tool("pipeline_cancel",
                     "Cancel/abandon a pipeline; requests cancellation of any in-flight roundtable "
                     "child run.",
                     cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                                 "\"pipeline_id\":{\"type\":\"integer\",\"description\":\"Pipeline "
                                 "id.\"}},\"required\":[\"pipeline_id\"]}")));
   }
