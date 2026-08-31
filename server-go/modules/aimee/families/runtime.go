package families

import store "github.com/JBailes/aimee/server-go/modules/aimee"

// The runtime family: the small stores a running daemon keeps.
//
// Bodies live in runtime_state.go, runtime_caches.go and runtime_fsnap.go;
// this file is only the wiring.

// EventRuntime and StageRuntime are from the catalog: ref 30's kind block.
const (
	EventRuntime uint32 = 11783
	StageRuntime uint32 = 7
)

const (
	opRuntimeStateSet    = 1
	opRuntimeStateGet    = 2
	opRuntimeStateAddInt = 3

	opProjectCloneUpsert        = 4
	opProjectCloneGet           = 5
	opProjectCloneDelete        = 6
	opProjectCloneList          = 7
	opProjectCloneListByProject = 8

	opLocalOperatorUpsert    = 9
	opLocalOperatorGet       = 10
	opLocalOperatorGetActive = 11
	opLocalOperatorSetActive = 12
	opLocalOperatorDelete    = 13
	opLocalOperatorList      = 14

	opEnvCapabilitySet  = 15
	opEnvCapabilityGet  = 16
	opEnvCapabilityList = 17

	opMaintenanceStateLoad = 18
	opMaintenanceStateSave = 19

	opModelCatalogIsFresh = 20
	opModelCatalogGet     = 21
	opModelCatalogReplace = 22
	opModelPriceGet       = 23
	opModelPriceSet       = 24
	opModelPriceDelete    = 25

	opWorkingProfileObserve    = 26
	opWorkingProfileList       = 27
	opWorkingProfileGet        = 28
	opWorkingProfileResetField = 29

	opToolAvailabilitySet    = 30
	opToolAvailabilityGet    = 31
	opToolAvailabilityDelete = 32
	opToolAvailabilityList   = 33

	opContextCacheGet        = 34
	opContextCachePut        = 35
	opContextCacheInvalidate = 36

	opContextSnapshotInsert          = 37
	opContextSnapshotCountMinSamples = 38
	opContextSnapshotIDsMinSamples   = 39
	opContextSnapshotCountForMemory  = 40
	opContextSnapshotSessions        = 41
	opContextSnapshotHasMemory       = 42

	opAgentCacheGet = 43
	opAgentCachePut = 44

	opWebPageGet          = 45
	opWebPagePut          = 46
	opWebPageDrop         = 47
	opWebPageCanonicalURL = 48

	opFsnapCreate      = 49
	opFsnapGetOrCreate = 50
	opFsnapRecordFile  = 51
	opFsnapPrune       = 52
	opFsnapList        = 53
	opFsnapRestore     = 54
	opFsnapGet         = 55

	opDecisionRecord = 56

	opOSVCacheGet    = 57
	opOSVCacheUpsert = 58
	opOSVCacheList   = 59
	opOSVAudit       = 60

	opContextSnapshotInsertTurn = 61
	opContextSnapshotActivation = 62
)

// Runtime is the family, ready to be bound to kind 11787.
var Runtime = store.Family{
	Name:  "runtime",
	Event: EventRuntime,
	Stage: StageRuntime,
	Ops: map[uint32]store.Op{
		// --- key/value state ---
		opRuntimeStateSet:    {Name: "runtime_state_set", Args: 2, Tx: true, Run: runtimeStateSet},
		opRuntimeStateGet:    {Name: "runtime_state_get", Cells: 2, Args: 1, Run: runtimeStateGet},
		opRuntimeStateAddInt: {Name: "runtime_state_add_int", Args: 2, Tx: true, Run: runtimeStateAddInt},

		// --- project clones ---
		opProjectCloneUpsert: {Name: "project_clone_upsert", Args: 5, Tx: true, Run: projectCloneUpsert},
		opProjectCloneGet:    {Name: "project_clone_get", Cells: 6, Args: 1, Run: projectCloneGet},
		opProjectCloneDelete: {Name: "project_clone_delete", Args: 1, Tx: true, Run: projectCloneDelete},
		opProjectCloneList: {
			Name: "project_clone_list", Cells: projectCloneCells, Args: 1, Run: projectCloneList,
		},
		opProjectCloneListByProject: {
			Name: "project_clone_list_by_project", Cells: projectCloneCells, Args: 2,
			Run: projectCloneListByProject,
		},

		// --- the local operator ---
		opLocalOperatorUpsert: {
			Name: "local_operator_upsert", Args: 4, Tx: true, Run: localOperatorUpsert,
		},
		opLocalOperatorGet:       {Name: "local_operator_get", Cells: 5, Args: 1, Run: localOperatorGet},
		opLocalOperatorGetActive: {Name: "local_operator_get_active", Cells: 5, Args: 0, Run: localOperatorGetActive},
		opLocalOperatorSetActive: {
			Name: "local_operator_set_active", Args: 1, Tx: true, Run: localOperatorSetActive,
		},
		opLocalOperatorDelete: {Name: "local_operator_delete", Args: 1, Tx: true, Run: localOperatorDelete},
		opLocalOperatorList: {
			Name: "local_operator_list", Cells: localOperatorCells, Args: 1, Run: localOperatorList,
		},

		// --- environment capabilities ---
		opEnvCapabilitySet: {Name: "env_capability_set", Args: 2, Tx: true, Run: envCapabilitySet},
		opEnvCapabilityGet: {Name: "env_capability_get", Cells: 2, Args: 1, Run: envCapabilityGet},
		opEnvCapabilityList: {
			Name: "env_capability_list", Cells: envCapabilityCells, Args: 1, Run: envCapabilityList,
		},

		// --- maintenance bookkeeping ---
		opMaintenanceStateLoad: {Name: "maintenance_state_load", Cells: 6, Args: 1, Run: maintenanceStateLoad},
		opMaintenanceStateSave: {
			Name: "maintenance_state_save", Args: 7, Tx: true, Run: maintenanceStateSave,
		},

		// --- the model catalog and its prices ---
		opModelCatalogIsFresh: {Name: "model_catalog_is_fresh", Args: 2, Run: modelCatalogIsFresh},
		opModelCatalogGet: {
			Name: "model_catalog_get", Cells: modelCatalogCells, Args: 1, Run: modelCatalogGet,
		},
		opModelCatalogReplace: {
			// Args: -1 -- variable. It carries a provider plus a repeated block
			// of models, so there is no single number the dispatch check could
			// hold it to; the operation validates its own shape. Declaring 1
			// refused every call that carried a model at all.
			Name: "model_catalog_replace", Args: -1, Tx: true, Run: modelCatalogReplace,
		},
		opModelPriceGet:    {Name: "model_price_get", Cells: 2, Args: 1, Run: modelPriceGet},
		opModelPriceSet:    {Name: "model_price_set", Args: 3, Tx: true, Run: modelPriceSet},
		opModelPriceDelete: {Name: "model_price_delete", Args: 1, Tx: true, Run: modelPriceDelete},

		// --- working profile observations ---
		opWorkingProfileObserve: {
			Name: "working_profile_local_observe", Args: 5, Tx: true, Run: workingProfileLocalObserve,
		},
		opWorkingProfileList: {
			Name: "working_profile_local_list", Cells: workingProfileCells, Args: 1,
			Run: workingProfileLocalList,
		},
		opWorkingProfileGet: {Name: "working_profile_local_get", Cells: 5, Args: 1, Run: workingProfileLocalGet},
		opWorkingProfileResetField: {
			Name: "working_profile_local_reset_field", Args: 1, Tx: true,
			Run: workingProfileLocalResetField,
		},

		// --- local tool availability ---
		opToolAvailabilitySet: {
			Name: "tool_local_availability_set", Args: 3, Tx: true, Run: toolAvailabilitySet,
		},
		opToolAvailabilityGet: {Name: "tool_local_availability_get", Cells: 4, Args: 1, Run: toolAvailabilityGet},
		opToolAvailabilityDelete: {
			Name: "tool_local_availability_delete", Args: 1, Tx: true, Run: toolAvailabilityDelete,
		},
		opToolAvailabilityList: {
			Name: "tool_local_availability_list", Cells: toolAvailCells, Args: 1,
			Run: toolAvailabilityList,
		},

		// --- the context cache ---
		opContextCacheGet: {Name: "context_cache_get", Cells: 2, Args: 1, Run: contextCacheGet},
		opContextCachePut: {Name: "context_cache_put", Args: 2, Tx: true, Run: contextCachePut},
		opContextCacheInvalidate: {
			Name: "context_cache_invalidate", Args: 0, Tx: true, Run: contextCacheInvalidate,
		},

		// --- context snapshots ---
		opContextSnapshotInsert: {
			Name: "context_snapshot_insert", Args: 3, Tx: true, Run: contextSnapshotInsert,
		},
		opContextSnapshotCountMinSamples: {
			Name: "context_snapshot_count_min_samples", Args: 1,
			Run: oneCount(contextSnapshotCountMinSQL),
		},
		opContextSnapshotIDsMinSamples: {
			Name: "context_snapshot_ids_min_samples", Cells: 1, Args: 2,
			Run: contextSnapshotIDsMinSamples,
		},
		opContextSnapshotCountForMemory: {
			Name: "context_snapshot_count_for_memory", Args: 1,
			Run: oneCount(contextSnapshotCountForMemorySQL),
		},
		opContextSnapshotSessions: {
			Name: "context_snapshot_sessions_for_memory", Cells: 1, Args: 2,
			Run: contextSnapshotSessionsForMemory,
		},
		opContextSnapshotHasMemory: {
			Name: "context_snapshot_has_memory", Args: 1, Run: contextSnapshotHasMemory,
		},
		opContextSnapshotInsertTurn: {
			Name: "context_snapshot_insert_turn", Args: 4, Tx: true, Run: contextSnapshotInsertTurn,
		},
		opContextSnapshotActivation: {
			Name: "context_snapshot_activation", Cells: 1, Args: 2, Tx: true,
			Run: contextSnapshotActivation,
		},

		// --- the agent cache ---
		opAgentCacheGet: {Name: "agent_cache_get", Args: 2, Run: agentCacheGet},
		opAgentCachePut: {Name: "agent_cache_put", Args: 3, Tx: true, Run: agentCachePut},

		// --- the web page cache ---
		opWebPageGet:          {Name: "web_page_get", Cells: 3, Args: 1, Tx: true, Run: webPageGet},
		opWebPagePut:          {Name: "web_page_put", Args: 3, Tx: true, Run: webPagePut},
		opWebPageDrop:         {Name: "web_page_drop", Args: 1, Tx: true, Run: webPageDrop},
		opWebPageCanonicalURL: {Name: "web_page_canonical_url", Args: 1, Cells: 2, Run: webPageCanonicalURL},

		// --- file snapshots ---
		opFsnapCreate:      {Name: "fsnap_create", Args: 3, Tx: true, Run: fsnapCreate},
		opFsnapGetOrCreate: {Name: "fsnap_get_or_create", Args: 3, Tx: true, Run: fsnapGetOrCreate},
		opFsnapRecordFile:  {Name: "fsnap_record_file", Args: 2, Tx: true, Run: fsnapRecordFile},
		opFsnapPrune:       {Name: "fsnap_prune", Args: 2, Tx: true, Run: fsnapPrune},
		opFsnapList:        {Name: "fsnap_list", Cells: fsnapCells, Args: 2, Run: fsnapList},
		opFsnapRestore:     {Name: "fsnap_restore", Cells: 2, Args: 1, Run: fsnapRestore},
		opFsnapGet:         {Name: "fsnap_get", Cells: 6, Args: 1, Run: fsnapGet},

		// --- decisions ---
		opDecisionRecord: {Name: "decision_record", Args: 3, Tx: true, Run: decisionRecord},

		// --- the OSV advisory cache ---
		opOSVCacheGet:    {Name: "mcp_osv_cache_get", Cells: 8, Args: 4, Run: osvCacheGet},
		opOSVCacheUpsert: {Name: "mcp_osv_cache_upsert", Args: 5, Tx: true, Run: osvCacheUpsert},
		opOSVCacheList:   {Name: "mcp_osv_cache_list", Cells: osvCells, Args: 1, Run: osvCacheList},
		opOSVAudit:       {Name: "mcp_osv_audit", Args: 7, Tx: true, Run: osvAudit},
	},
}
