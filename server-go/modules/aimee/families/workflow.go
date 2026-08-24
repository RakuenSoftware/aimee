package families

import store "github.com/JBailes/aimee/server-go/modules/aimee"

// The workflow family: execution plans and their steps, traces, pipelines,
// roadmap dispatches, and the workflow-engine session binding.
//
// Bodies live in workflow_plans.go, workflow_runtime.go and
// workflow_roadmap.go; this file is only the wiring.

// EventWorkflow and StageWorkflow are from the catalog: ref 30's kind block.
const (
	EventWorkflow uint32 = 11787
	StageWorkflow uint32 = 11
)

const (
	opTraceInsert          = 1
	opTraceCountForSession = 2
	opTraceListRecent      = 3
	opTraceGet             = 4
	opTraceListToolCalls   = 5
	opTraceListAfterID     = 6

	opWFEBind               = 7
	opWFEBindingGet         = 8
	opWFEUnbind             = 9
	opWFELeaseRenew         = 10
	opWFELeaseExpiryGet     = 11
	opWFELeaseStaleWorkItem = 12
	opWFELeaseReclaimStale  = 13

	opPipelineCreate     = 14
	opPipelineGet        = 15
	opPipelineUpdate     = 16
	opPipelineLinkPlan   = 17
	opPipelineLinkJob    = 18
	opPipelineCancel     = 19
	opPipelineListActive = 20

	opRoadmapDispatchUpsert    = 21
	opRoadmapDispatchGet       = 22
	opRoadmapDispatchSetStatus = 23
	opRoadmapDispatchSetPhase  = 24

	opRoadmapUnitEnsure         = 25
	opRoadmapUnitGet            = 26
	opRoadmapUnitSetState       = 27
	opRoadmapUnitClaim          = 28
	opRoadmapUnitHeartbeat      = 29
	opRoadmapUnitFinish         = 30
	opRoadmapUnitSetCoordJob    = 31
	opRoadmapUnitIncrementVerif = 32
	opRoadmapUnitSelectNext     = 33

	opPlanCreate              = 34
	opPlanGet                 = 35
	opPlanListIDs             = 36
	opPlanExists              = 37
	opPlanCountSteps          = 38
	opPlanListRunningIDs      = 39
	opPlanListRecentSummaries = 40
	opPlanSetStatus           = 41
	opPlanCancelByID          = 42
	opPlanCancelStale         = 43
	opStepSetStatus           = 44
	opStepSetStatusOutput     = 45
	opStepCancelActiveForPlan = 46
	opStepCancelOrphans       = 47
	opStepEvidenceInsert      = 48
	opStepEvidenceGetLatest   = 49
)

// Workflow is the family, ready to be bound to kind 11784.
var Workflow = store.Family{
	Name:  "workflow",
	Event: EventWorkflow,
	Stage: StageWorkflow,
	Ops: map[uint32]store.Op{
		// --- execution traces ---
		opTraceInsert:          {Name: "execution_trace_insert", Args: 9, Tx: true, Run: traceInsert},
		opTraceCountForSession: {Name: "execution_trace_count_for_session", Args: 1, Run: traceCountForSession},
		opTraceListRecent: {
			Name: "execution_trace_list_recent", Cells: traceRecentCells, Args: 1,
			Run: traceListRecent,
		},
		opTraceGet: {Name: "execution_trace_get", Cells: 10, Args: 1, Run: traceGet},
		opTraceListToolCalls: {
			Name: "execution_trace_list_tool_calls", Cells: traceToolCallCells, Args: 1,
			Run: traceListToolCalls,
		},
		opTraceListAfterID: {
			Name: "execution_trace_list_after_id", Cells: traceAfterIDCells, Args: 2,
			Run: traceListAfterID,
		},

		// --- the workflow-engine binding ---
		opWFEBind:           {Name: "wfe_bind", Args: 3, Tx: true, Run: wfeBind},
		opWFEBindingGet:     {Name: "wfe_binding_get", Cells: 2, Args: 1, Run: wfeBindingGet},
		opWFEUnbind:         {Name: "wfe_unbind", Args: 1, Tx: true, Run: wfeUnbind},
		opWFELeaseRenew:     {Name: "wfe_lease_renew", Args: 2, Tx: true, Run: wfeLeaseRenew},
		opWFELeaseExpiryGet: {Name: "wfe_lease_expiry_get", Args: 1, Run: wfeLeaseExpiryGet},
		opWFELeaseStaleWorkItem: {
			Name: "wfe_lease_stale_work_items", Cells: 1, Args: 1, Run: wfeLeaseStaleWorkItems,
		},
		opWFELeaseReclaimStale: {
			Name: "wfe_lease_reclaim_stale", Args: 0, Tx: true, Run: wfeLeaseReclaimStale,
		},

		// --- pipelines ---
		opPipelineCreate: {Name: "pipeline_create", Args: 3, Tx: true, Run: pipelineCreate},
		opPipelineGet:    {Name: "pipeline_get", Cells: 12, Args: 1, Run: pipelineGet},
		opPipelineUpdate: {Name: "pipeline_update", Args: 9, Tx: true, Run: pipelineUpdate},
		opPipelineLinkPlan: {
			Name: "pipeline_link_plan", Args: 2, Tx: true, Run: linkPipeline(pipelineLinkPlanSQL),
		},
		opPipelineLinkJob: {
			Name: "pipeline_link_job", Args: 2, Tx: true, Run: linkPipeline(pipelineLinkJobSQL),
		},
		opPipelineCancel: {Name: "pipeline_cancel", Args: 1, Tx: true, Run: pipelineCancel},
		opPipelineListActive: {
			Name: "pipeline_list_active", Cells: pipelineCells, Args: 1, Run: pipelineListActive,
		},

		// --- roadmap dispatches ---
		opRoadmapDispatchUpsert: {
			Name: "roadmap_dispatch_upsert", Args: 4, Tx: true, Run: roadmapDispatchUpsert,
		},
		opRoadmapDispatchGet: {Name: "roadmap_dispatch_get", Cells: 10, Args: 1, Run: roadmapDispatchGet},
		opRoadmapDispatchSetStatus: {
			Name: "roadmap_dispatch_set_status", Args: 3, Tx: true, Run: roadmapDispatchSetStatus,
		},
		opRoadmapDispatchSetPhase: {
			Name: "roadmap_dispatch_set_phase", Args: 2, Tx: true, Run: roadmapDispatchSetPhase,
		},

		// --- roadmap units ---
		opRoadmapUnitEnsure:   {Name: "roadmap_unit_ensure", Args: 4, Tx: true, Run: roadmapUnitEnsure},
		opRoadmapUnitGet:      {Name: "roadmap_unit_get", Cells: 17, Args: 2, Run: roadmapUnitGet},
		opRoadmapUnitSetState: {Name: "roadmap_unit_set_state", Args: 3, Tx: true, Run: roadmapUnitSetState},
		opRoadmapUnitClaim:    {Name: "roadmap_unit_claim", Args: 4, Tx: true, Run: roadmapUnitClaim},
		opRoadmapUnitHeartbeat: {
			Name: "roadmap_unit_heartbeat", Args: 2, Tx: true, Run: roadmapUnitHeartbeat,
		},
		opRoadmapUnitFinish: {Name: "roadmap_unit_finish", Args: 5, Tx: true, Run: roadmapUnitFinish},
		opRoadmapUnitSetCoordJob: {
			Name: "roadmap_unit_set_coord_job", Args: 3, Tx: true, Run: roadmapUnitSetCoordJob,
		},
		opRoadmapUnitIncrementVerif: {
			Name: "roadmap_unit_increment_verify_attempts", Args: 2, Tx: true,
			Run: roadmapUnitIncrementVerifyAttempts,
		},
		opRoadmapUnitSelectNext: {
			Name: "roadmap_unit_select_next", Cells: 2, Args: 1, Run: roadmapUnitSelectNext,
		},

		// --- execution plans ---
		opPlanCreate: {Name: "execution_plan_create", Args: 3, Tx: true, Run: planCreate},
		opPlanGet:    {Name: "execution_plan_get", Cells: 549, Args: 1, Run: planGet},
		opPlanListIDs: {
			Name: "execution_plan_list_ids", Cells: 1, Args: 1, Run: planListIDs,
		},
		opPlanExists:     {Name: "execution_plan_exists", Args: 1, Run: planExists},
		opPlanCountSteps: {Name: "execution_plan_count_steps", Args: 1, Run: planCountSteps},
		opPlanListRunningIDs: {
			Name: "execution_plan_list_running_ids", Cells: 1, Args: 1, Run: planListRunningIDs,
		},
		opPlanListRecentSummaries: {
			Name: "execution_plan_list_recent_summaries", Cells: planSummaryCells, Args: 1,
			Run: planListRecentSummaries,
		},
		opPlanSetStatus:   {Name: "execution_plan_set_status", Args: 2, Tx: true, Run: planSetStatus},
		opPlanCancelByID:  {Name: "execution_plan_cancel_by_id", Args: 2, Tx: true, Run: planCancelByID},
		opPlanCancelStale: {Name: "execution_plan_cancel_stale", Args: 2, Tx: true, Run: planCancelStale},

		// --- plan steps ---
		opStepSetStatus: {Name: "plan_step_set_status", Args: 2, Tx: true, Run: stepSetStatus},
		opStepSetStatusOutput: {
			Name: "plan_step_set_status_output", Args: 3, Tx: true, Run: stepSetStatusOutput,
		},
		opStepCancelActiveForPlan: {
			Name: "plan_step_cancel_active_for_plan", Args: 1, Tx: true, Run: stepCancelActiveForPlan,
		},
		opStepCancelOrphans: {
			Name: "plan_step_cancel_orphans", Args: 0, Tx: true, Run: stepCancelOrphans,
		},
		opStepEvidenceInsert: {
			Name: "step_evidence_insert", Args: 6, Tx: true, Run: stepEvidenceInsert,
		},
		opStepEvidenceGetLatest: {Name: "step_evidence_get_latest", Cells: 4, Args: 1, Run: stepEvidenceGetLatest},
	},
}
