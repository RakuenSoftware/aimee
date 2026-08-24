package families

import store "github.com/JBailes/aimee/server-go/modules/aimee"

// The conversation family: everything a session accumulates while it runs --
// working memory, tool-event chains, context windows, payload rewrite state,
// user memories, and clarification sessions.
//
// The bodies live in conversation_wm.go, conversation_rewrite.go,
// conversation_context.go, conversation_windows.go, conversation_user_memory.go
// and conversation_clarify.go; this file is only the wiring.

// EventConversation and StageConversation are from the catalog: ref 30's kind
// block, principal ref 3.
const (
	EventConversation uint32 = 11779
	StageConversation uint32 = 3
)

// Operation ids, from the catalog. The gap at 36 is the catalog's, not a
// mistake here: no operation carries that id.
const (
	opRewriteStateGet   = 1
	opRewriteStateSet   = 2
	opWMSet             = 3
	opWMGet             = 4
	opWMList            = 5
	opWMAssembleContext = 6
	opRewriteRecord     = 7
	opWMSearchSessionID = 8

	opConvRecordEvent   = 9
	opConvSetChainID    = 10
	opConvInsertChain   = 11
	opConvPendingEvents = 12
	opConvListChains    = 13
	opConvChainEvents   = 14
	opConvSearchChains  = 15
	opConvStateGet      = 16
	opConvStateUpdate   = 17

	opWindowScanState         = 18
	opWindowSessionID         = 19
	opWindowCreateRaw         = 20
	opWindowAddTerm           = 21
	opWindowAddFile           = 22
	opWindowIDsByTier         = 23
	opWindowCandidatesByTerms = 24
	opWindowListFiles         = 25
	opWindowIndexSummary      = 26
	opWindowLexicalHits       = 27
	opWindowSetTier           = 28
	opWindowPruneTerms        = 29
	opWindowDeleteAllFiles    = 30
	opWindowPruneFiles        = 31
	opWindowsDeleteAfterTurn  = 32

	opUserMemoryListRecall = 33
	opUserMemoryAny        = 34
	opUserMemoryUpsert     = 35

	opClarifyStart        = 37
	opClarifyGet          = 38
	opClarifyAnswer       = 39
	opClarifyScore        = 40
	opClarifyWeakestDim   = 41
	opClarifyNextQuestion = 42
	opClarifyCrystallize  = 43

	opWMDelete = 44
	opWMClear  = 45
	opWMGC     = 46
)

// Conversation is the family, ready to be bound to kind 11779.
var Conversation = store.Family{
	Name:  "conversation",
	Event: EventConversation,
	Stage: StageConversation,
	Ops: map[uint32]store.Op{
		// --- payload rewrite state ---
		opRewriteStateGet: {Name: "rewrite_state_get", Cells: 11, Args: 1, Run: rewriteStateGet},
		opRewriteStateSet: {Name: "rewrite_state_set", Args: 11, Tx: true, Run: rewriteStateSet},
		opRewriteRecord:   {Name: "rewrite_record", Args: 6, Tx: true, Run: rewriteRecord},

		// --- working memory ---
		opWMSet:             {Name: "wm_set", Args: 5, Tx: true, Run: wmSet},
		opWMGet:             {Name: "wm_get", Cells: 8, Args: 2, Run: wmGet},
		opWMList:            {Name: "wm_list", Cells: 8, Args: 3, Run: wmList},
		opWMAssembleContext: {Name: "wm_assemble_context", Args: 1, Run: wmAssembleContext},
		opWMSearchSessionID: {Name: "wm_search_session_ids", Cells: 1, Args: 2, Run: wmSearchSessionIDs},
		opWMDelete:          {Name: "wm_delete", Args: 2, Tx: true, Run: wmDelete},
		opWMClear:           {Name: "wm_clear", Args: 1, Tx: true, Run: wmClear},
		opWMGC:              {Name: "wm_gc", Args: 0, Tx: true, Run: wmGC},

		// --- tool events and chains ---
		opConvRecordEvent:   {Name: "conv_record_event", Args: 5, Tx: true, Run: convRecordEvent},
		opConvSetChainID:    {Name: "conv_set_chain_id", Args: 3, Tx: true, Run: convSetChainID},
		opConvInsertChain:   {Name: "conv_insert_chain", Args: 7, Tx: true, Run: convInsertChain},
		opConvPendingEvents: {Name: "conv_pending_events", Cells: 8, Args: 2, Run: convPendingEvents},
		opConvListChains:    {Name: "conv_list_chains", Cells: 10, Args: 2, Run: convListChains},
		opConvChainEvents:   {Name: "conv_chain_events", Cells: 8, Args: 2, Run: convChainEvents},
		opConvSearchChains:  {Name: "conv_search_chains", Cells: 10, Args: 3, Run: convSearchChains},
		opConvStateGet:      {Name: "conv_state_get", Cells: 3, Args: 1, Run: convStateGet},
		opConvStateUpdate:   {Name: "conv_state_update", Args: 4, Tx: true, Run: convStateUpdate},

		// --- context windows ---
		//
		// The two searches are variadic: one leading field for the row limit,
		// then the terms. They validate their own shape, which is what a
		// negative arity means.
		opWindowScanState:         {Name: "window_scan_state", Cells: 2, Args: 1, Run: windowScanState},
		opWindowSessionID:         {Name: "window_session_id", Args: 1, Run: windowSessionID},
		opWindowCreateRaw:         {Name: "window_create_raw", Args: 4, Tx: true, Run: windowCreateRaw},
		opWindowAddTerm:           {Name: "window_add_term", Args: 2, Tx: true, Run: windowAddTerm},
		opWindowAddFile:           {Name: "window_add_file", Args: 2, Tx: true, Run: windowAddFile},
		opWindowIDsByTier:         {Name: "window_ids_by_tier", Cells: 1, Args: 3, Run: windowIDsByTier},
		opWindowCandidatesByTerms: {Name: "window_candidates_by_terms", Cells: 6, Args: -1, Run: windowCandidatesByTerms},
		opWindowListFiles:         {Name: "window_list_files", Cells: 1, Args: 2, Run: windowListFiles},
		opWindowIndexSummary:      {Name: "window_index_summary", Args: 2, Run: windowIndexSummary},
		opWindowLexicalHits:       {Name: "window_lexical_hits", Cells: 2, Args: -1, Run: windowLexicalHits},
		opWindowSetTier:           {Name: "window_set_tier", Args: 2, Tx: true, Run: windowSetTier},
		opWindowPruneTerms:        {Name: "window_prune_terms", Args: 2, Tx: true, Run: windowPruneTerms},
		opWindowDeleteAllFiles:    {Name: "window_delete_all_files", Args: 1, Tx: true, Run: windowDeleteAllFiles},
		opWindowPruneFiles:        {Name: "window_prune_files", Args: 2, Tx: true, Run: windowPruneFiles},
		opWindowsDeleteAfterTurn:  {Name: "windows_delete_after_turn", Args: 2, Tx: true, Run: windowsDeleteAfterTurn},

		// --- user memories ---
		opUserMemoryListRecall: {Name: "user_memory_list_recall", Cells: 5, Args: 2, Run: userMemoryListRecall},
		opUserMemoryAny:        {Name: "user_memory_any", Args: 0, Run: userMemoryAny},
		opUserMemoryUpsert:     {Name: "user_memory_upsert", Args: 6, Tx: true, Run: userMemoryUpsert},

		// --- clarification sessions ---
		//
		// The last four touch no table: they are pure functions of a session the
		// caller sends in full. See the note at the top of
		// conversation_clarify.go.
		opClarifyStart:        {Name: "clarify_start", Cells: 48, Args: 1, Tx: true, Run: clarifyStart},
		opClarifyGet:          {Name: "clarify_get", Cells: 48, Args: 1, Run: clarifyGet},
		opClarifyAnswer:       {Name: "clarify_answer", Cells: 48, Args: 2, Tx: true, Run: clarifyAnswer},
		opClarifyScore:        {Name: "clarify_score", Args: clarifySessFields, Run: clarifyScoreOp},
		opClarifyWeakestDim:   {Name: "clarify_weakest_dim", Args: clarifySessFields, Run: clarifyWeakestDimOp},
		opClarifyNextQuestion: {Name: "clarify_next_question", Cells: 2, Args: clarifySessFields, Run: clarifyNextQuestionOp},
		opClarifyCrystallize:  {Name: "clarify_crystallize", Args: clarifySessFields, Run: clarifyCrystallizeOp},
	},
}
