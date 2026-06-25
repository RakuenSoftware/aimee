/* test_mcp_tools_golden.inc: AUTO-GENERATED golden for mcp_build_tools_list()'s
 * built-in tool surface (name + sorted schema property keys + required), captured
 * via the DUMP_TOOLS path in test_mcp_client_registry.c. Regenerate after an
 * intentional tool change: DUMP_TOOLS=1 ./unit-test-mcp-client-registry 2>&1. */
#define MCP_TOOLS_GOLDEN_COUNT 47
#define MCP_TOOLS_GOLDEN \
   "ask_user {choices,question} req:question\n" \
   "ast_grep_search {lang,path,pattern} req:lang,pattern\n" \
   "attempt {approach,command,filter,lesson,outcome,task_context} req:command\n" \
   "autopilot {action,job_id,pipeline_id,plan_depth,plan_id,task} req:action\n" \
   "background {action,command,cwd,id,tail_lines} req:action\n" \
   "clarify {answer,command,description,session_id} req:command\n" \
   "dashboard_metrics {} req:\n" \
   "delegate {branch,cwd,persona,prompt,role} req:persona,prompt,role\n" \
   "delegate_reply {content,delegation_id} req:content,delegation_id\n" \
   "delegate_status {job_id} req:job_id\n" \
   "describe_tool {name} req:name\n" \
   "diagnose {command,content,diagnosis_id,hypothesis_id,rank,source,stance,symptom} req:command\n" \
   "ensemble_review {brief,diff,rounds,turns} req:diff\n" \
   "epistemic_directive {anchor_entity,anchor_file,cause,command,id,limit,note,priority,question,resolution_memory_id,state,suppress,topic,valid_until} req:command\n" \
   "find_symbol {identifier} req:identifier\n" \
   "find_tools {limit,query} req:\n" \
   "get_help {topic} req:\n" \
   "get_identity {} req:\n" \
   "git {action,async,base,body,branch,command,count,depth,diff_stat,files,force,index,job_id,message,mirror,mode,name,number,path,prune,rebase,ref,remote,source,staged,stat_only,state,title,url,wait} req:command\n" \
   "graph {command,entity,episode_key,limit,query} req:command\n" \
   "host {command,name} req:command\n" \
   "index {command,file_path,paths,project,symbol} req:command\n" \
   "job {command,job_id,max_concurrent,plan_id} req:command\n" \
   "learning {command,correction_text,description,evidence_refs,limit,polarity,signal_type,sink,state,target_key,target_memory_id,title,workflow_project,workflow_signal_type} req:command\n" \
   "list_curiosity_items {limit,state} req:\n" \
   "lsp {col,command,file,line,workspace} req:command\n" \
   "memory {command,confidence,content,dry_run,force,handle,id,key,kind,memory_id,modes,query,reason,tier,verb} req:command\n" \
   "memory_recall {limit_tokens,session_start,task_hint} req:\n" \
   "note {command,content,limit,query,tag,tags,title} req:command\n" \
   "payload_rewrite_status {} req:\n" \
   "pdf_inspect_structure {document_key,project} req:document_key,project\n" \
   "pdf_open_neighbors {chunk_id,project} req:chunk_id,project\n" \
   "pdf_open_page {document_key,page_no,project} req:document_key,page_no,project\n" \
   "pdf_search_chunks {max_results,project,query} req:project,query\n" \
   "pipeline {admin,artifact,base_branch,brief,command,done_bar,head_branch,idea,operator_principal,pipeline_id,questions,reason,remote,repo_root,state,verdict,worktree_path} req:command\n" \
   "prospective_memory {action_text,anchor_entity,anchor_file,command,id,limit,recurrence,state,trigger_text,valid_until} req:command\n" \
   "recall {block_type,command,limit,limit_tokens,query,since} req:command\n" \
   "roadmap {command,roadmap_id} req:command\n" \
   "rules {command,reason,text} req:command\n" \
   "search_docs {max_results,query} req:query\n" \
   "search_memory {filter,query} req:query\n" \
   "send_message {target,text} req:target,text\n" \
   "session {around_message_id,assignments,chain_id,channel,command,id,include_sources,limit,message,query,reason,session_id,speaker,template,window} req:command\n" \
   "skill_manage {absorbed_into,action,content,cwd,file_path,name,new_string,old_string,replace_all} req:action,name\n" \
   "store_workflow {project,rule,signal_type} req:rule,signal_type\n" \
   "task_list {limit,session_id,state} req:\n" \
   "work {command,status_filter} req:command\n"
