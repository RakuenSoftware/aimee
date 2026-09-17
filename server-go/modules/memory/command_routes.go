package memory

import "github.com/JBailes/aimee/server-go/bus"

// A route is declared and invoked from the same table. Internal view builders
// remain callable on the authenticated module bus without becoming RPC actions.
type commandRoute struct {
	verb, summary string
	handler       func(handlerOptions, bus.ModuleInvocation, string, commandArgs) ([]byte, bus.ModuleStatus)
	public        bool
}

var kbCommandRoutes = []commandRoute{
	{"delete", "Delete memories.", handleMutationCommand, true},
	{"update", "Update memories.", handleMutationCommand, true},
	{"touch", "Touch memories.", handleMutationCommand, true},
	{"reject", "Reject memories.", handleMutationCommand, true},
	{"restore", "Restore memories.", handleMutationCommand, true},
	{"upsert_workflow", "Upsert workflow memories.", handleMutationCommand, true},

	{"briefing", "Briefing memories.", handleRuntimeCommand, true},
	{"alerts", "Alerts memories.", handleRuntimeCommand, true},
	{"assemble_context", "Assemble context memories.", handleRuntimeCommand, true},
	{"compact_windows", "Compact windows memories.", handleRuntimeCommand, true},
	{"query_edges", "Query edges memories.", handleRuntimeCommand, true},
	{"check_drift", "Check drift memories.", handleRuntimeCommand, true},

	{"get", "Get memories.", handleRecordCommand, true},
	{"list", "List memories.", handleRecordCommand, true},
	{"fact_history", "Fact history memories.", handleRecordCommand, true},
	{"top_l2_facts", "Top l2 facts memories.", handleRecordCommand, true},
	{"load_eval_corpus", "Load eval corpus memories.", handleRecordCommand, true},
	{"list_session_scope_priority", "List session scope priority memories.", handleRecordCommand, true},
	{"list_session_scope_priority_like", "List session scope priority like memories.", handleRecordCommand, true},
	{"search_facts_patterns_by_keyword", "Search facts patterns by keyword memories.", handleRecordCommand, true},
	{"scope_visibility_rank", "Scope visibility rank memories.", handleScopeCommand, true},
	{"tag_workspace", "Tag workspace memories.", handleScopeCommand, true},
	{"tag_scope", "Tag scope memories.", handleScopeCommand, true},

	{"recall", "Recall memories for the current turn.", func(o handlerOptions, i bus.ModuleInvocation, _ string, a commandArgs) ([]byte, bus.ModuleStatus) {
		return handleRecallCommand(o, i, a)
	}, true},
	{"maintenance_run", "Run memory maintenance.", handleMaintenanceCommand, true},
	{"lint", "Inspect memory consistency.", handleMaintenanceCommand, true},
	{"prospective_list", "List commitments.", handleProspectiveCommand, true},
	{"prospective_create", "Create a commitment.", handleProspectiveCommand, true},
	{"prospective_match", "Match commitments to a turn.", handleProspectiveCommand, true},
	{"prospective_complete", "Complete a commitment.", handleProspectiveCommand, true},
	{"prospective_mark_triggered", "Mark a commitment triggered.", handleProspectiveCommand, true},
	{"prospective_sweep_expired", "Expire commitments.", handleProspectiveCommand, true},
	{"prospective_dashboard", "", handleProspectiveCommand, false},
	{"prospective_briefing", "", handleProspectiveCommand, false},
	{"directive_create", "Create an epistemic directive.", handleDirectiveCommand, true},
	{"directive_resolve", "Resolve an epistemic directive.", handleDirectiveCommand, true},
	{"directive_suppress", "Suppress an epistemic directive.", handleDirectiveCommand, true},
	{"directive_sweep_expired", "Expire epistemic directives.", handleDirectiveCommand, true},
	{"directive_list", "List epistemic directives.", handleDirectiveCommand, true},
	{"directive_dashboard", "", handleDirectiveCommand, false},
	{"directive_briefing", "", handleDirectiveCommand, false},
	{"entity_profile", "Read an entity profile.", handleDomainCommand, true},
	{"entity_edges", "Read an entity's relations.", handleDomainCommand, true},
	{"search_graph", "Search memory relations.", handleDomainCommand, true},
	{"search_graph_as_of", "Search historical memory relations.", handleDomainCommand, true},
	{"get_episode", "Read a memory episode.", handleDomainCommand, true},
	{"get_provenance", "Read memory provenance.", handleDomainCommand, true},
	{"link_query", "List memory links.", handleDomainCommand, true},
	{"link_create", "Create a memory link.", handleDomainCommand, true},
	{"link_delete", "Delete a memory link.", handleDomainCommand, true},
	{"list_conflicts", "List memory conflicts.", handleDomainCommand, true},
	{"query_health", "Read memory health.", handleDomainCommand, true},
	{"stats", "Read memory statistics.", handleDomainCommand, true},
	{"stats_dashboard", "", handleDomainCommand, false},
	{"key_exists", "Check whether a memory key exists.", handleQueryCommand, true},
	{"find_id_by_key_kind", "Find a memory id by key and kind.", handleQueryCommand, true},
	{"list_low_effectiveness", "List low-effectiveness memories.", handleQueryCommand, true},
	{"list_unused_l2", "List unused observations.", handleQueryCommand, true},
	{"list_superseded_keys", "List versioned memory keys.", handleQueryCommand, true},
	{"review_list", "Review memory history.", handleQueryCommand, true},
	{"review_console", "", handleQueryCommand, false},
	{"set_artifact", "Update a memory artifact reference.", handleQueryCommand, true},
	{"effectiveness_stats", "Read memory effectiveness.", handleQueryCommand, true},
}

func describeCommandRoutes(options handlerOptions, invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	commands := []bus.CommandDefinition{}
	// The Server's shared-KB transport is still being migrated. Do not shadow
	// those handlers with private-only commands under the same public name.
	if options.placement == PlacementKB {
		for _, r := range kbCommandRoutes {
			if r.public {
				commands = append(commands, bus.CommandDefinition{Group: "memory", Verb: r.verb, Summary: r.summary, Surfaces: SurfaceRPC, Visibility: MCPDiscoverable})
			}
		}
	}
	response, err := bus.EncodeCommandDeclaration(request, StageCommand, commands)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return response, bus.ModuleStatusOK
}
