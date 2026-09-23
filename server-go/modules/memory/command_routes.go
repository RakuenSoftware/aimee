package memory

import "github.com/JBailes/aimee/server-go/bus"

// A route is declared and invoked from the same table. Internal view builders
// remain callable on the authenticated module bus without becoming RPC actions.
type commandRoute struct {
	group, verb, summary string
	handler              func(handlerOptions, bus.ModuleInvocation, string, commandArgs) ([]byte, bus.ModuleStatus)
	public               bool
}

var sharedCommandRoutes = []commandRoute{
	{"memory", "verify_receipt", "Compare a supplied prepared receipt with optional exact payload bytes.", handleReceiptVerification, true},
	{"memory", "pack", "Manage memory profile packs.", handlePackCommand, true},
	{"memory", "screen_content", "Screen content before transmission or export.", handleScreenCommand, true},
}

var kbCommandRoutes = []commandRoute{
	{"memory", "revalidate_sources", "Recheck scoped source versions before provider handoff.", handleSourceRevalidation, true},
	{"memory", "correction_proposals", "Inspect scoped correction drafts outside recall.", handleCorrectionProposalCommand, true},
	{"memory", "review_correction", "Approve or reject the exact scoped correction draft.", handleCorrectionProposalCommand, true},
	{"memory", "benchmark", "Evaluate live scoped retrieval with complete per-case receipts.", handleLiveBenchmark, true},
	{"memory", "audit", "Audit labelled queries against actual scoped candidate order.", handleLabelAudit, true},
	{"memory", "calibrate", "Fit diagnostic multipliers without changing the serving ranker.", handleLabelAudit, true},
	{"memory", "search_assertions", "Search visible semantic assertions across valid and belief time.", handleEvidenceCommand, true},
	{"memory", "assemble_typed_context", "Assemble scoped evidence with per-channel context budgets.", handleEvidenceCommand, true},
	{"memory", "reflect", "Reflect on scoped memories and optionally propose a draft rule.", handleReflectCommand, true},
	{"memory", "checkpoint", "Snapshot facts or restore checkpoint content into scoped memory.", handleCheckpointCommand, true},
	{"memory", "ontology", "List the ontology or walk its scoped graph.", handleOntologyCommand, true},
	{"memory", "cognify", "Derive scoped claims and relations from a memory.", handleCognifyCommand, true},
	{"memory", "cognify_drain", "Process a bounded batch of scoped cognification jobs.", handleCognifyCommand, true},
	{"memory", "cognify_status", "Read scoped cognification queue status.", handleCognifyCommand, true},
	{"memory", "embed", "Embed current memory records.", handleRepairCommand, true},
	{"memory", "reembed_start", "Stage a resumable embedding version.", handleReembedCommand, true},
	{"memory", "reembed_status", "Inspect versioned embedding progress.", handleReembedCommand, true},
	{"memory", "reembed_cutover", "Atomically activate a complete embedding version.", handleReembedCommand, true},
	{"memory", "reembed_rollback", "Restore a retained current embedding version.", handleReembedCommand, true},
	{"memory", "find_facts", "Find scoped facts with the configured retrieval policy.", handleRecordCommand, true},
	{"maintenance", "fold_session", "Fold a scoped session and capture learning evidence.", handleRuntimeCommand, true},
	{"memory", "scene_list", "List visible memory scenes.", handleDomainCommand, true},
	{"memory", "scene_show", "List visible scene members.", handleDomainCommand, true},
	{"memory", "verify", "Verify deployed memory vectors and indexing health.", handleVerifyCommand, true},
	{"memory", "episode_cards", "Read scoped session episode cards.", handleRuntimeCommand, true},
	{"memory", "repair", "Repair memory vectors and retry failed indexing.", handleRepairCommand, true},
	{"memory", "rebuild", "Rebuild the memory vector index.", handleVectorCommand, true},
	{"memory", "reindex", "Rebuild derived memory indexes.", handleVectorCommand, true},
	{"memory", "ask", "Answer from scoped memory evidence.", handleAnswerCommand, true},
	{"memory", "diagnose_scoped", "Explain scoped memory retrieval and optionally record its trace.", handleDiagnosticCommand, true},
	{"memory", "explain_match", "Explain a memory's match to a query.", handleDiagnosticCommand, true},
	{"maintenance", "anti_pattern_extract_from_feedback", "Learn anti-patterns from feedback.", handleRuntimeCommand, true},
	{"maintenance", "anti_pattern_extract_from_failures", "Learn anti-patterns from failed decisions.", handleRuntimeCommand, true},
	{"maintenance", "anti_pattern_escalate", "Promote repeated anti-patterns into rules.", handleRuntimeCommand, true},
	{"maintenance", "memory_learn_style", "Learn response style preferences.", handleRuntimeCommand, true},
	{"maintenance", "scan_conversations", "Import conversation observations.", handleRuntimeCommand, true},
	{"memory", "find_facts_visible", "Find facts in the active project, workspace and global scope.", handleRecordCommand, true},
	{"memory", "find_facts_scoped", "Find facts in an explicit scope.", handleRecordCommand, true},
	{"relations", "schema_list", "Read the enforced memory relation schema.", handleSchemaCommand, true},
	{"memory", "search", "Search conversation memories.", handleRuntimeCommand, true},
	{"memory", "episode_card_generate", "Build a scoped session episode card.", handleRuntimeCommand, true},
	{"memory", "export_jsonl", "Export memories with their scopes.", handleRuntimeCommand, true},
	{"memory", "decisions_export_jsonl", "Export memory decisions.", handleRuntimeCommand, true},
	{"memory", "supersede", "Replace a memory while preserving history.", handleSupersedeCommand, true},
	{"memory", "store", "Store a memory and schedule fact extraction.", handleStoreCommand, true},
	{"memory", "delete", "Delete memories.", handleMutationCommand, true},
	{"memory", "update", "Update memories.", handleMutationCommand, true},
	{"memory", "touch", "Touch memories.", handleMutationCommand, true},
	{"memory", "reject", "Reject memories.", handleMutationCommand, true},
	{"memory", "restore", "Restore memories.", handleMutationCommand, true},
	{"memory", "upsert_workflow", "Upsert workflow memories.", handleMutationCommand, true},

	{"memory", "briefing", "Briefing memories.", handleRuntimeCommand, true},
	{"memory", "alerts", "Alerts memories.", handleRuntimeCommand, true},
	{"facts", "retract", "Withdraw a typed fact at verified authority.", handleFactCommand, true},
	{"memory", "context_block", "Build scoped context and recalled facts.", handleRuntimeCommand, true},
	{"memory", "facts", "Recall scoped typed facts for a turn.", handleRuntimeCommand, true},
	{"memory", "assemble_context", "Assemble context memories.", handleRuntimeCommand, true},
	{"memory", "compact_windows", "Compact windows memories.", handleRuntimeCommand, true},
	{"memory", "query_edges", "Query edges memories.", handleRuntimeCommand, true},
	{"memory", "check_drift", "Check drift memories.", handleRuntimeCommand, true},

	{"memory", "get", "Get memories.", handleRecordCommand, true},
	{"memory", "list", "List memories.", handleRecordCommand, true},
	{"memory", "fact_history", "Fact history memories.", handleRecordCommand, true},
	{"memory", "top_l2_facts", "Top l2 facts memories.", handleRecordCommand, true},
	{"memory", "load_eval_corpus", "Load eval corpus memories.", handleRecordCommand, true},
	{"memory", "list_session_scope_priority", "List session scope priority memories.", handleRecordCommand, true},
	{"memory", "list_session_scope_priority_like", "List session scope priority like memories.", handleRecordCommand, true},
	{"memory", "search_facts_patterns_by_keyword", "Search facts patterns by keyword memories.", handleRecordCommand, true},
	{"memory", "scope_visibility_rank", "Scope visibility rank memories.", handleScopeCommand, true},
	{"memory", "tag_workspace", "Tag workspace memories.", handleScopeCommand, true},
	{"memory", "tag_scope", "Tag scope memories.", handleScopeCommand, true},

	{"memory", "recall", "Recall memories for the current turn.", func(o handlerOptions, i bus.ModuleInvocation, _ string, a commandArgs) ([]byte, bus.ModuleStatus) {
		return handleRecallCommand(o, i, a)
	}, true},
	{"memory", "maintenance_run", "Run memory maintenance.", handleMaintenanceCommand, true},
	{"memory", "lint", "Inspect memory consistency.", handleMaintenanceCommand, true},
	{"memory", "prospective_list", "List commitments.", handleProspectiveCommand, true},
	{"memory", "prospective_create", "Create a commitment.", handleProspectiveCommand, true},
	{"memory", "prospective_match", "Match commitments to a turn.", handleProspectiveCommand, true},
	{"memory", "prospective_complete", "Complete a commitment.", handleProspectiveCommand, true},
	{"memory", "prospective_mark_triggered", "Mark a commitment triggered.", handleProspectiveCommand, true},
	{"memory", "prospective_sweep_expired", "Expire commitments.", handleProspectiveCommand, true},
	{"memory", "prospective_dashboard", "", handleProspectiveCommand, false},
	{"memory", "prospective_briefing", "", handleProspectiveCommand, false},
	{"memory", "directive_create", "Create an epistemic directive.", handleDirectiveCommand, true},
	{"memory", "directive_resolve", "Resolve an epistemic directive.", handleDirectiveCommand, true},
	{"memory", "directive_suppress", "Suppress an epistemic directive.", handleDirectiveCommand, true},
	{"memory", "directive_sweep_expired", "Expire epistemic directives.", handleDirectiveCommand, true},
	{"memory", "directive_list", "List epistemic directives.", handleDirectiveCommand, true},
	{"memory", "directive_dashboard", "", handleDirectiveCommand, false},
	{"memory", "directive_briefing", "", handleDirectiveCommand, false},
	{"memory", "entity_profile", "Read an entity profile.", handleDomainCommand, true},
	{"memory", "entity_edges", "Read an entity's relations.", handleDomainCommand, true},
	{"memory", "search_graph", "Search memory relations.", handleDomainCommand, true},
	{"memory", "search_graph_as_of", "Search historical memory relations.", handleDomainCommand, true},
	{"memory", "get_episode", "Read a memory episode.", handleDomainCommand, true},
	{"memory", "get_provenance", "Read memory provenance.", handleDomainCommand, true},
	{"memory", "link_query", "List memory links.", handleDomainCommand, true},
	{"memory", "link_create", "Create a memory link.", handleDomainCommand, true},
	{"memory", "link_delete", "Delete a memory link.", handleDomainCommand, true},
	{"memory", "list_conflicts", "List memory conflicts.", handleDomainCommand, true},
	{"memory", "query_health", "Read memory health.", handleDomainCommand, true},
	{"memory", "stats", "Read memory statistics.", handleDomainCommand, true},
	{"memory", "stats_dashboard", "", handleDomainCommand, false},
	{"memory", "key_exists", "Check whether a memory key exists.", handleQueryCommand, true},
	{"memory", "find_id_by_key_kind", "Find a memory id by key and kind.", handleQueryCommand, true},
	{"memory", "list_low_effectiveness", "List low-effectiveness memories.", handleQueryCommand, true},
	{"memory", "list_unused_l2", "List unused observations.", handleQueryCommand, true},
	{"memory", "list_superseded_keys", "List versioned memory keys.", handleQueryCommand, true},
	{"memory", "review_list", "Review memory history.", handleQueryCommand, true},
	{"memory", "review_console", "", handleQueryCommand, false},
	{"memory", "set_artifact", "Update a memory artifact reference.", handleQueryCommand, true},
	{"memory", "effectiveness_stats", "Read memory effectiveness.", handleQueryCommand, true},
}

func describeCommandRoutes(options handlerOptions, invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	commands := []bus.CommandDefinition{{Group: "memory", Verb: "embed_text", Summary: "Embed trusted host input."}, {Group: "memory", Verb: "runtime", Summary: "Read host runtime views."}}
	// The Server's shared-KB transport is still being migrated. Do not shadow
	// those handlers with private-only commands under the same public name.
	routes := sharedCommandRoutes
	if options.placement == PlacementKB {
		routes = append(append([]commandRoute{}, sharedCommandRoutes...), kbCommandRoutes...)
	}
	for _, r := range routes {
		if r.public {
			commands = append(commands, bus.CommandDefinition{Group: r.group, Verb: r.verb, Summary: r.summary, Surfaces: SurfaceRPC, Visibility: MCPDiscoverable})
		}
	}
	response, err := bus.EncodeCommandDeclaration(request, StageCommand, commands)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return response, bus.ModuleStatusOK
}
