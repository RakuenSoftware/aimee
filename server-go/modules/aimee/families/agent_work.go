package families

import store "github.com/JBailes/aimee/server-go/modules/aimee"

// The agent-work family: the cognify queue, the agent log, trigger runs,
// coordination jobs and their tasks, and cron jobs.
//
// Bodies live in agent_work_log.go, agent_work_coord.go and
// agent_work_cron.go; this file is only the wiring.

// EventAgentWork and StageAgentWork are from the catalog: ref 30's kind block.
const (
	EventAgentWork uint32 = 11780
	StageAgentWork uint32 = 4
)

const (
	opCognifyEnqueue   = 1
	opCognifyStatus    = 2
	opCognifyClaimNext = 3
	opCognifyMark      = 4

	opAgentLogInsert             = 5
	opAgentLogListRecent         = 6
	opAgentLogListBySession      = 7
	opAgentLogSearchSessions     = 8
	opAgentLogCountPerRole       = 9
	opAgentLogFailuresSince      = 10
	opAgentLogListRecentErrors   = 11
	opAgentLogDelegationPatterns = 12
	opAgentLogFailureSeeds       = 13
	opAgentLogMetricsByRole      = 14
	opAgentLogAgentStats         = 15
	opAgentLogHUDSummary         = 16
	opAgentLogSessionOutcome     = 17
	opAgentLogPrometheus         = 18
	opAgentLogStats              = 19

	opTriggerInsert    = 20
	opTriggerStatusSet = 21
	opTriggerGet       = 22
	opTriggerListJSON  = 23

	opCoordJobCreate               = 24
	opCoordTaskAdd                 = 25
	opCoordTaskClaimNext           = 26
	opCoordTaskComplete            = 27
	opCoordTaskFail                = 28
	opCoordTaskCompleteOwned       = 29
	opCoordTaskFailOwned           = 30
	opCoordTaskRelease             = 31
	opCoordTaskReleaseBounded      = 32
	opCoordTaskReleaseBoundedOwned = 33
	opCoordOwnerRecover            = 34
	opCoordJobGet                  = 35
	opCoordTaskList                = 36
	opCoordJobCancel               = 37
	opCoordJobRefreshStatus        = 38
	opCoordJobFileConflict         = 39
	opCoordJobListRecent           = 40
	opCoordJobListActiveIDs        = 41
	opCoordTaskGetDispatch         = 42

	opCronJobUpsert         = 43
	opCronJobGet            = 44
	opCronJobLoad           = 45
	opCronJobSetEnabled     = 46
	opCronJobSetEnabledAll  = 47
	opCronJobDelete         = 48
	opCronJobRecordRun      = 49
	opCronJobListJSON       = 50
	opCronJobHistoryJSON    = 51
	opCronJobLatestOutput   = 52
	opCronJobLastOutputHash = 53
)

// AgentWork is the family, ready to be bound to kind 11786.
var AgentWork = store.Family{
	Name:  "agent_work",
	Event: EventAgentWork,
	Stage: StageAgentWork,
	Ops: map[uint32]store.Op{
		// --- the cognify queue ---
		opCognifyEnqueue:   {Name: "cognify_enqueue", Args: 1, Tx: true, Run: cognifyEnqueue},
		opCognifyStatus:    {Name: "cognify_status", Cells: 5, Args: 0, Run: cognifyStatus},
		opCognifyClaimNext: {Name: "cognify_claim_next", Cells: 9, Args: 0, Tx: true, Run: cognifyClaimNext},
		opCognifyMark:      {Name: "cognify_mark", Args: 3, Tx: true, Run: cognifyMark},

		// --- the agent log ---
		opAgentLogInsert: {Name: "agent_log_insert", Args: 11, Tx: true, Run: agentLogInsert},
		opAgentLogListRecent: {
			Name: "agent_log_list_recent", Cells: agentLogRowCells, Args: 1, Run: agentLogListRecent,
		},
		opAgentLogListBySession: {
			Name: "agent_log_list_by_session", Cells: agentLogRowCells, Args: 2,
			Run: agentLogListBySession,
		},
		opAgentLogSearchSessions: {
			Name: "agent_log_search_sessions_by_role", Cells: 1, Args: 2,
			Run: agentLogSearchSessionsByRole,
		},
		opAgentLogCountPerRole: {
			Name: "agent_log_count_per_role", Cells: roleCountCells, Args: 2,
			Run: agentLogCountPerRole,
		},
		opAgentLogFailuresSince: {
			Name: "agent_log_failures_since", Cells: agentLogFailureCells, Args: 2,
			Run: agentLogFailuresSince,
		},
		opAgentLogListRecentErrors: {
			Name: "agent_log_list_recent_errors", Cells: 1, Args: 2,
			Run: agentLogListRecentErrors,
		},
		opAgentLogDelegationPatterns: {
			Name: "agent_log_delegation_patterns", Cells: agentLogPatternCells, Args: 3,
			Run: agentLogDelegationPatterns,
		},
		opAgentLogFailureSeeds: {
			Name: "agent_log_failure_seeds", Cells: agentLogSeedCells, Args: 3,
			Run: agentLogFailureSeeds,
		},
		opAgentLogMetricsByRole: {
			Name: "agent_log_metrics_by_role", Cells: agentLogMetricCells, Args: 1,
			Run: agentLogMetricsByRole,
		},
		opAgentLogAgentStats: {
			Name: "agent_log_agent_stats", Cells: agentLogStatsCells, Args: 2,
			Run: agentLogAgentStats,
		},
		opAgentLogHUDSummary:     {Name: "agent_log_hud_summary", Cells: 10, Args: 1, Run: agentLogHUDSummary},
		opAgentLogSessionOutcome: {Name: "agent_log_session_outcome", Cells: 2, Args: 1, Run: agentLogSessionOutcome},
		opAgentLogPrometheus: {
			Name: "agent_log_prometheus", Cells: agentLogPromCells, Args: 1, Run: agentLogPrometheus,
		},
		opAgentLogStats: {Name: "agent_log_stats", Cells: 6, Args: 1, Run: agentLogStats},

		// --- trigger runs ---
		opTriggerInsert:    {Name: "trigger_insert", Args: 6, Tx: true, Run: triggerInsert},
		opTriggerStatusSet: {Name: "trigger_status_set", Args: 4, Tx: true, Run: triggerStatusSet},
		opTriggerGet:       {Name: "trigger_get", Cells: 12, Args: 1, Run: triggerGet},
		opTriggerListJSON:  {Name: "trigger_list_json", Args: 1, Run: triggerListJSON},

		// --- coordination jobs and tasks ---
		opCoordJobCreate:     {Name: "coord_job_create", Args: 2, Tx: true, Run: coordJobCreate},
		opCoordTaskAdd:       {Name: "coord_task_add", Args: 7, Tx: true, Run: coordTaskAdd},
		opCoordTaskClaimNext: {Name: "coord_task_claim_next", Cells: 11, Args: 2, Tx: true, Run: coordTaskClaimNext},
		opCoordTaskComplete: {
			Name: "coord_task_complete", Args: 2, Tx: true,
			Run: finishTask(coordTaskCompleteSQL, false),
		},
		opCoordTaskFail: {
			Name: "coord_task_fail", Args: 2, Tx: true, Run: finishTask(coordTaskFailSQL, false),
		},
		opCoordTaskCompleteOwned: {
			Name: "coord_task_complete_owned", Args: 3, Tx: true,
			Run: finishTask(coordTaskCompleteOwnedSQL, true),
		},
		opCoordTaskFailOwned: {
			Name: "coord_task_fail_owned", Args: 3, Tx: true,
			Run: finishTask(coordTaskFailOwnedSQL, true),
		},
		opCoordTaskRelease: {
			Name: "coord_task_release", Args: 1, Tx: true, Run: coordTaskRelease,
		},
		opCoordTaskReleaseBounded: {
			Name: "coord_task_release_bounded", Args: 2, Tx: true, Run: coordTaskReleaseBounded,
		},
		opCoordTaskReleaseBoundedOwned: {
			Name: "coord_task_release_bounded_owned", Args: 3, Tx: true,
			Run: coordTaskReleaseBoundedOwned,
		},
		opCoordOwnerRecover: {Name: "coord_owner_recover", Cells: 2, Args: 2, Tx: true, Run: coordOwnerRecover},
		opCoordJobGet:       {Name: "coord_job_get", Cells: 10, Args: 1, Run: coordJobGet},
		opCoordTaskList: {
			Name: "coord_task_list", Cells: coordTaskCells, Args: 2, Run: coordTaskList,
		},
		opCoordJobCancel: {Name: "coord_job_cancel", Args: 1, Tx: true, Run: coordJobCancel},
		opCoordJobRefreshStatus: {
			Name: "coord_job_refresh_status", Args: 1, Tx: true, Run: coordJobRefreshStatus,
		},
		opCoordJobFileConflict: {
			Name: "coord_job_file_conflict", Args: 2, Run: coordJobFileConflict,
		},
		opCoordJobListRecent: {
			Name: "coord_job_list_recent", Cells: coordJobCells, Args: 1, Run: coordJobListRecent,
		},
		opCoordJobListActiveIDs: {
			Name: "coord_job_list_active_ids", Cells: 1, Args: 1, Run: coordJobListActiveIDs,
		},
		opCoordTaskGetDispatch: {Name: "coord_task_get_dispatch", Cells: 5, Args: 1, Run: coordTaskGetDispatch},

		// --- cron jobs ---
		opCronJobUpsert: {Name: "cron_job_upsert", Args: cronCells, Tx: true, Run: cronJobUpsert},
		opCronJobGet:    {Name: "cron_job_get", Cells: 22, Args: 1, Run: cronJobGet},
		opCronJobLoad: {
			Name: "cron_job_load", Cells: cronCells, Args: 2, Run: cronJobLoad,
		},
		opCronJobSetEnabled: {
			Name: "cron_job_set_enabled", Args: 2, Tx: true, Run: cronJobSetEnabled,
		},
		opCronJobSetEnabledAll: {
			Name: "cron_job_set_enabled_all", Args: 1, Tx: true, Run: cronJobSetEnabledAll,
		},
		opCronJobDelete:    {Name: "cron_job_delete", Args: 1, Tx: true, Run: cronJobDelete},
		opCronJobRecordRun: {Name: "cron_job_record_run", Args: 7, Tx: true, Run: cronJobRecordRun},
		opCronJobListJSON:  {Name: "cron_job_list_json", Args: 0, Run: cronJobListJSON},
		opCronJobHistoryJSON: {
			Name: "cron_job_history_json", Args: 2, Run: cronJobHistoryJSON,
		},
		opCronJobLatestOutput: {
			Name: "cron_job_latest_output", Args: 1, Run: cronJobLatestOutput,
		},
		opCronJobLastOutputHash: {
			Name: "cron_job_last_output_hash", Args: 1, Run: cronJobLastOutputHash,
		},
	},
}
