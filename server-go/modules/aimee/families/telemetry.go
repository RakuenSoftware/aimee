package families

import store "github.com/JBailes/aimee/server-go/modules/aimee"

// The telemetry family: what a run cost, what it did, and what went wrong.
//
// Bodies live in telemetry_token_audit.go, telemetry_events.go,
// telemetry_insights.go and telemetry_diagnose.go; this file is only the
// wiring.

// EventTelemetry and StageTelemetry are from the catalog: ref 30's kind block.
const (
	EventTelemetry uint32 = 11784
	StageTelemetry uint32 = 8
)

// Operation ids, from the catalog. The gaps at 45-47 are the catalog's.
const (
	opTokenAuditInsert            = 1
	opTokenAuditEnsureIdemIndex   = 2
	opTokenAuditCostForDelegation = 3
	opTokenAuditCostForDelegEx    = 4
	opTokenAuditSessionSplit      = 5
	opTokenAuditTotals            = 6
	opTokenAuditSpendBreakdown    = 7
	opTokenAuditByRole            = 8
	opTokenAuditByTool            = 9
	opTokenAuditByModel           = 10
	opTokenAuditBySource          = 11
	opTokenAuditListDashboard     = 12

	opInsightsByPlatform      = 13
	opInsightsTopSessions     = 14
	opInsightsDelegatesByRole = 15

	opCostFoldRecord = 16
	opCostFoldTotal  = 17

	opInteractionEventRecord            = 18
	opInteractionEventListUnreflected   = 19
	opInteractionEventListForSession    = 20
	opInteractionEventListPromotionFeed = 21
	opInteractionEventMarkReflected     = 22
	opInteractionEventMarkPromoted      = 23
	opInteractionEventEvictIfNeeded     = 24

	opGuardrailEventInsert             = 25
	opGuardrailEventCounts7d           = 26
	opGuardrailEventSessionAdvisoryCnt = 27
	opGuardrailEventList               = 28

	opEvalResultInsert      = 29
	opEvalFailedTasksRecent = 30
	opEvalPassedTasksRecent = 31
	opEvalResultsList       = 32

	opDiagnoseStart          = 33
	opDiagnoseAddObservation = 34
	opDiagnoseAddHypothesis  = 35
	opDiagnoseAddEvidence    = 36
	opDiagnoseAddProbe       = 37
	opDiagnoseGet            = 38
	opDiagnoseList           = 39
	opDiagnoseListItems      = 40
	opDiagnoseListHypotheses = 41
	opDiagnoseRankHypotheses = 42
	opDiagnoseConclude       = 43
	opDiagnoseAbandon        = 44
	opDiagnoseSuggestProbes  = 48
)

// Telemetry is the family, ready to be bound to kind 11783.
var Telemetry = store.Family{
	Name:  "telemetry",
	Event: EventTelemetry,
	Stage: StageTelemetry,
	Ops: map[uint32]store.Op{
		// --- token accounting ---
		opTokenAuditInsert: {
			Name: "token_audit_insert", Args: 23, Tx: true, Run: tokenAuditInsert,
		},
		opTokenAuditEnsureIdemIndex: {
			Name: "token_audit_ensure_idem_index", Args: 0, Run: tokenAuditEnsureIdemIndex,
		},
		opTokenAuditCostForDelegation: {
			Name: "token_audit_cost_for_delegation", Args: 1,
			Run: oneCost(tokenAuditCostForDelegationSQL),
		},
		opTokenAuditCostForDelegEx: {
			Name: "token_audit_cost_for_delegation_ex", Args: 1,
			Run: oneCost(tokenAuditCostForDelegationExSQL),
		},
		opTokenAuditSessionSplit:   {Name: "token_audit_session_split", Cells: 12, Args: 1, Run: tokenAuditSessionSplit},
		opTokenAuditTotals:         {Name: "token_audit_totals", Cells: 6, Args: 1, Run: tokenAuditTotals},
		opTokenAuditSpendBreakdown: {Name: "token_audit_spend_breakdown", Cells: 5, Args: 1, Run: tokenAuditSpendBreakdown},
		opTokenAuditByRole: {
			Name: "token_audit_by_role", Cells: tokenAuditGroupedCells, Args: 2,
			Run: groupedSpend(tokenAuditByRoleSQL),
		},
		opTokenAuditByTool: {
			Name: "token_audit_by_tool", Cells: tokenAuditGroupedCells, Args: 2,
			Run: groupedSpend(tokenAuditByToolSQL),
		},
		opTokenAuditByModel: {
			Name: "token_audit_by_model", Cells: tokenAuditGroupedCells, Args: 2,
			Run: groupedSpend(tokenAuditByModelSQL),
		},
		opTokenAuditBySource: {
			Name: "token_audit_by_source", Cells: tokenAuditGroupedCells, Args: 2,
			Run: groupedSpend(tokenAuditBySourceSQL),
		},
		opTokenAuditListDashboard: {
			Name: "token_audit_list_dashboard", Cells: tokenAuditDashboardCells, Args: 1,
			Run: tokenAuditListDashboard,
		},

		// --- insights ---
		opInsightsByPlatform: {
			Name: "insights_by_platform", Cells: insightsPlatformCells, Args: 2,
			Run: insightsByPlatform,
		},
		opInsightsTopSessions: {
			Name: "insights_top_sessions", Cells: insightsSessionCells, Args: 2,
			Run: insightsTopSessions,
		},
		opInsightsDelegatesByRole: {
			Name: "insights_delegates_by_role", Cells: insightsDelegateCells, Args: 2,
			Run: insightsDelegatesByRole,
		},

		// --- cost folding ---
		opCostFoldRecord: {Name: "cost_fold_record", Args: 4, Tx: true, Run: costFoldRecord},
		opCostFoldTotal:  {Name: "cost_fold_total", Args: 1, Run: costFoldTotal},

		// --- interaction events ---
		//
		// The two marking operations are variadic: the wire carries only ids,
		// up to 512 of them, and they validate their own shape.
		opInteractionEventRecord: {
			Name: "interaction_event_record", Args: 5, Tx: true, Run: interactionEventRecord,
		},
		opInteractionEventListUnreflected: {
			Name: "interaction_event_list_unreflected", Cells: 7, Args: 2,
			Run: interactionEventListUnreflected,
		},
		opInteractionEventListForSession: {
			Name: "interaction_event_list_for_session", Cells: 7, Args: 2,
			Run: interactionEventListForSession,
		},
		opInteractionEventListPromotionFeed: {
			Name: "interaction_event_list_promotion_feed", Cells: 7, Args: 1,
			Run: interactionEventListPromotionFeed,
		},
		opInteractionEventMarkReflected: {
			Name: "interaction_event_mark_reflected", Args: -1, Tx: true,
			Run: markInteraction(interactionMarkReflectedSQL),
		},
		opInteractionEventMarkPromoted: {
			Name: "interaction_event_mark_promoted", Args: -1, Tx: true,
			Run: markInteraction(interactionMarkPromotedSQL),
		},
		opInteractionEventEvictIfNeeded: {
			Name: "interaction_event_evict_if_needed", Args: 1, Tx: true,
			Run: interactionEventEvictIfNeeded,
		},

		// --- guardrail events ---
		opGuardrailEventInsert: {
			Name: "guardrail_event_insert", Args: 12, Tx: true, Run: guardrailEventInsert,
		},
		opGuardrailEventCounts7d: {Name: "guardrail_event_counts_7d", Cells: 4, Args: 0, Run: guardrailEventCounts7d},
		opGuardrailEventSessionAdvisoryCnt: {
			Name: "guardrail_event_session_advisory_count", Args: 1,
			Run: guardrailEventSessionAdvisoryCount,
		},
		opGuardrailEventList: {
			Name: "guardrail_event_list", Cells: guardrailEventCells, Args: 2,
			Run: guardrailEventList,
		},

		// --- evaluation results ---
		opEvalResultInsert: {
			Name: "eval_result_insert", Args: 19, Tx: true, Run: evalResultInsert,
		},
		opEvalFailedTasksRecent: {
			Name: "eval_failed_tasks_recent", Cells: 2, Args: 1, Run: evalFailedTasksRecent,
		},
		opEvalPassedTasksRecent: {
			Name: "eval_passed_tasks_recent", Cells: 1, Args: 1, Run: evalPassedTasksRecent,
		},
		opEvalResultsList: {
			Name: "eval_results_list", Cells: evalCells, Args: 2, Run: evalResultsList,
		},

		// --- diagnoses ---
		opDiagnoseStart: {
			Name: "diagnose_start", Args: 1, Tx: true, Run: diagnoseStart,
		},
		opDiagnoseAddObservation: {
			Name: "diagnose_add_observation", Args: 3, Tx: true, Run: diagnoseAddObservation,
		},
		opDiagnoseAddHypothesis: {
			Name: "diagnose_add_hypothesis", Args: 2, Tx: true, Run: diagnoseAddHypothesis,
		},
		opDiagnoseAddEvidence: {
			Name: "diagnose_add_evidence", Args: 6, Tx: true, Run: diagnoseAddEvidence,
		},
		opDiagnoseAddProbe: {
			Name: "diagnose_add_probe", Args: 3, Tx: true, Run: diagnoseAddProbe,
		},
		opDiagnoseGet:  {Name: "diagnose_get", Cells: 7, Args: 1, Run: diagnoseGet},
		opDiagnoseList: {Name: "diagnose_list", Cells: diagnosisCells, Args: 1, Run: diagnoseList},
		opDiagnoseListItems: {
			Name: "diagnose_list_items", Cells: diagnosisItemCells, Args: 2,
			Run: listDiagnosisItems(diagnoseListItemsSQL),
		},
		opDiagnoseListHypotheses: {
			Name: "diagnose_list_hypotheses", Cells: diagnosisItemCells, Args: 2,
			Run: listDiagnosisItems(diagnoseListHypothesesSQL),
		},
		opDiagnoseRankHypotheses: {
			Name: "diagnose_rank_hypotheses", Cells: diagnosisRankCells, Args: 2,
			Run: diagnoseRankHypotheses,
		},
		opDiagnoseConclude: {
			Name: "diagnose_conclude", Args: 3, Tx: true, Run: diagnoseConclude,
		},
		opDiagnoseAbandon: {
			Name: "diagnose_abandon", Args: 1, Tx: true, Run: diagnoseAbandon,
		},
		opDiagnoseSuggestProbes: {
			Name: "diagnose_suggest_probes", Cells: diagnosisProbeCells, Args: 2,
			Run: diagnoseSuggestProbes,
		},
	},
}
