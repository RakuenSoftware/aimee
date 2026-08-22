package db2

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// The fakes prove an operation handles the rows it is given. They cannot prove
// its statement is a statement: a column that does not exist, a function never
// created, a placeholder in the wrong dialect all pass a fake and fail a
// database. That is what this covers, and it is the reason the C side keeps a
// replay against real PostgreSQL -- the unit suites there drive stub backends
// for the same reason and miss the same things.
//
// Skipped without AIMEE_DB2_URL, so an ordinary `go test ./...` stays hermetic.
// Point it at the replay database, which scripts/db2_replay_env.sh creates:
//
//	bash scripts/db2_replay_env.sh
//	AIMEE_DB2_URL="postgres://aimee:aimee@$(ssh root@192.168.1.252 \
//	  'pct exec 9001 -- hostname -I' | tr -d ' \n')/aimee_db2_process_ci" \
//	  go test ./modules/db2/ -run Live
//
// The address is read rather than written down: the container takes a DHCP
// lease, so it changes whenever the container is recreated -- which it has
// been, repeatedly.
//
// Do not run these while scripts/db2_verify_batch.sh is running. They share one
// database with the C replay, and sharing it is not safe in either direction:
// the replay rebuilds the schema underneath a probe, a probe holds row locks
// the replay then blocks on, and the sequence restoration below writes to
// sequences the replay is drawing identities from. A probe run concurrent with
// a replay has already produced a replay failure that looked like a code
// defect and was not one.

func liveStore(t *testing.T) (Store, func()) {
	t.Helper()
	dsn := os.Getenv("AIMEE_DB2_URL")
	if dsn == "" {
		t.Skip("AIMEE_DB2_URL is unset; this test needs a real database")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatalf("parse AIMEE_DB2_URL: %v", err)
	}
	config.MaxConns = 2
	pool, err := pgxpool.NewWithConfig(context.Background(), config)
	if err != nil {
		t.Fatalf("open pool: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		t.Fatalf("ping: %v", err)
	}
	return &PoolStore{pool: pool}, pool.Close
}

// liveRequest is one ported operation and a request that must be answerable
// against a schema with no rows in it. Every read entry is a READ: it runs
// against a shared database and must not leave anything in it. Write entries
// run inside a transaction that always rolls back.
type liveRequest struct {
	name    string
	stage   uint32
	encode  func() ([]byte, error)
	decoded func(t *testing.T, body []byte)
	// repeat runs a write probe more than once inside its transaction, for an
	// operation whose second call is the one that used to fail.
	repeat int
	// seed runs inside the probe's transaction before the operation, for a
	// write whose interesting path needs rows to act on. A probe against an
	// empty schema proves the statement parses and binds; it does not prove the
	// statement matches anything, and for an operation like generation_publish
	// -- whose whole behaviour is a changed-row count -- that is most of what
	// there is to prove. Seeded rows roll back with the probe.
	seed []string
}

// The scope every scoped probe asks under: active, naming a workspace and a
// project nothing holds.
//
// Active is the point. An inactive scope short-circuits the filter before it
// reaches memory_scopes or memory_workspaces, so probing with one would prove
// the statement parses and nothing about the twelve subqueries the rank is
// built from -- which is most of what these statements are.
const (
	liveProbeScopeFlags   uint32 = 3
	liveProbeWorkspace           = "live-probe-workspace"
	liveProbeScopeProject        = "live-probe-project"
)

func liveReads() []liveRequest {
	return []liveRequest{
		{
			name:  "curator_invalidations_since",
			stage: db2contract.StageCuratorInvalidationsSince,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCuratorInvalidationsSinceRequest(0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCuratorInvalidationsSinceReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "prospective_get",
			stage: db2contract.StageProspectiveGet,
			encode: func() ([]byte, error) {
				// An identifier nothing holds: the answer is "not found", which
				// is what proves the statement ran rather than that a row exists.
				return db2contract.EncodeProspectiveGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeProspectiveGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatalf("found = %d for an identifier nothing holds", found)
				}
			},
		},
		{
			name:  "prospective_record_trigger",
			stage: db2contract.StageProspectiveRecordTrigger,
			encode: func() ([]byte, error) {
				// An UPDATE whose WHERE matches nothing: it runs, changes no row,
				// and leaves the database as it found it.
				return db2contract.EncodeProspectiveRecordTriggerRequest(2147483000, 0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeProspectiveRecordTriggerReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a statement that matched no row did not acknowledge")
				}
			},
		},
		{
			name:  "entity_list_active",
			stage: db2contract.StageEntityListActive,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityListActiveRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityListActiveReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_edge_co_targets",
			stage: db2contract.StageEntityEdgeCoTargets,
			encode: func() ([]byte, error) {
				// The union, the visibility projection and its join to
				// code_projection_generations all have to resolve. A fake
				// proves none of that.
				return db2contract.EncodeEntityEdgeCoTargetsRequest("replay-src", "mentions", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityEdgeCoTargetsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "code_index_project_list",
			stage:  db2contract.StageCodeIndexProjectList,
			encode: db2contract.EncodeCodeIndexProjectListRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCodeIndexProjectListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "audit_event_list",
			stage: db2contract.StageAuditEventList,
			encode: func() ([]byte, error) {
				// All three predicates, so the assembled statement is the widest
				// shape rather than the simplest one.
				return db2contract.EncodeAuditEventListRequest(
					"2000-01-01T00:00:00Z", "2099-01-01T00:00:00Z", "user", 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeAuditEventListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "demotion_candidates",
			stage: db2contract.StageDemotionCandidates,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDemotionCandidatesRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDemotionCandidatesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "directive_find_by_cause_topic",
			stage: db2contract.StageDirectiveFindByCauseTopic,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveFindByCauseTopicRequest(
					"retrieval_failure", "replay-topic")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeDirectiveFindByCauseTopicReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "memory_last_retro_scan",
			stage:  db2contract.StageMemoryLastRetroScan,
			encode: db2contract.EncodeMemoryLastRetroScanRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryLastRetroScanReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "project_last_scan",
			stage:  db2contract.StageProjectLastScan,
			encode: db2contract.EncodeProjectLastScanRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProjectLastScanReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "project_current_generation",
			stage: db2contract.StageProjectCurrentGeneration,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectCurrentGenerationRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProjectCurrentGenerationReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "project_fingerprint",
			stage: db2contract.StageProjectFingerprint,
			encode: func() ([]byte, error) {
				// md5, string_agg and the ordered aggregate all have to resolve.
				return db2contract.EncodeProjectFingerprintRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				fingerprint, err := db2contract.DecodeProjectFingerprintReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				// The statement coalesces, so even a project with no files
				// fingerprints as the md5 of the empty string -- 32 hex
				// characters, never nothing.
				if len(fingerprint) != 32 {
					t.Fatalf("fingerprint = %q, want 32 hex characters", fingerprint)
				}
			},
		},
		{
			name:   "bandit_decision_points",
			stage:  db2contract.StageBanditDecisionPoints,
			encode: db2contract.EncodeBanditDecisionPointsRequest,
			decoded: func(t *testing.T, body []byte) {
				encoded, err := db2contract.DecodeBanditDecisionPointsReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				var points []string
				if err := json.Unmarshal([]byte(encoded), &points); err != nil {
					t.Fatalf("the reply is not parseable JSON: %v (%q)", err, encoded)
				}
			},
		},
		{
			name:   "active_embedder_version",
			stage:  db2contract.StageActiveEmbedderVersion,
			encode: db2contract.EncodeActiveEmbedderVersionRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeActiveEmbedderVersionReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "visible_source_hash",
			stage: db2contract.StageVisibleSourceHash,
			encode: func() ([]byte, error) {
				return db2contract.EncodeVisibleSourceHashRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeVisibleSourceHashReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "projection_visible_id",
			stage: db2contract.StageProjectionVisibleID,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectionVisibleIDRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProjectionVisibleIDReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_profile_card",
			stage: db2contract.StageEntityProfileCard,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityProfileCardRequest("Replay-Entity")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityProfileCardReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "code_file_hash",
			stage: db2contract.StageCodeFileHash,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCodeFileHashRequest("replay-project", "src/main.c")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCodeFileHashReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "enrollment_list",
			stage: db2contract.StageEnrollmentList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEnrollmentListRequest(8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEnrollmentListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "match_error_keys",
			stage: db2contract.StageMatchErrorKeys,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMatchErrorKeysRequest("connection refused by peer")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMatchErrorKeysReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_ids_by_updated",
			stage: db2contract.StageMemoryIdsByUpdated,
			// Zero, which is the branch NULLIF covers. A plain LIMIT $1 would
			// answer empty here and the reply would look identical to a schema
			// with no memories in it, so this is the value worth probing.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryIdsByUpdatedRequest(0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryIdsByUpdatedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "unit_ids_for_memory",
			stage: db2contract.StageUnitIdsForMemory,
			encode: func() ([]byte, error) {
				return db2contract.EncodeUnitIdsForMemoryRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeUnitIdsForMemoryReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_relation_dates",
			stage: db2contract.StageMemoryRelationDates,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryRelationDatesRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryRelationDatesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_depends_on_keys",
			stage: db2contract.StageMemoryDependsOnKeys,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryDependsOnKeysRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryDependsOnKeysReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_session_content",
			stage: db2contract.StageMemorySessionContent,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySessionContentRequest("live-probe-session")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySessionContentReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_session_created_at",
			stage: db2contract.StageMemorySessionCreatedAt,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySessionCreatedAtRequest("live-probe-session")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySessionCreatedAtReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "runtime_state_get",
			stage: db2contract.StageRuntimeStateGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRuntimeStateGetRequest("live-probe-never-set")
			},
			decoded: func(t *testing.T, body []byte) {
				value, err := db2contract.DecodeRuntimeStateGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if value != "" {
					t.Fatalf("value = %q for a key nothing has set", value)
				}
			},
		},
		{
			name:  "count_embeddings_for_version",
			stage: db2contract.StageCountEmbeddingsForVersion,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCountEmbeddingsForVersionRequest("live-probe-v0")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCountEmbeddingsForVersionReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "lifecycle_get_state",
			stage: db2contract.StageLifecycleGetState,
			encode: func() ([]byte, error) {
				return db2contract.EncodeLifecycleGetStateRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				state, err := db2contract.DecodeLifecycleGetStateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if state != "" {
					t.Fatalf("state = %q for a memory nothing holds", state)
				}
			},
		},
		{
			name:  "memory_first_episode_card",
			stage: db2contract.StageMemoryFirstEpisodeCard,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryFirstEpisodeCardRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryFirstEpisodeCardReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "dedupe_by_key",
			stage: db2contract.StageDedupeByKey,
			// A READ entry because a dry run writes nothing, and it is the half
			// worth probing here: the CTE, the DISTINCT ON and the self-join all
			// have to resolve against the real schema, and a fake proves none of
			// that. The merging half is probed separately.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDedupeByKeyRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDedupeByKeyReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "ontology_eval_status",
			stage: db2contract.StageOntologyEvalStatus,
			encode: func() ([]byte, error) {
				return db2contract.EncodeOntologyEvalStatusRequest("live-probe-never-proposed")
			},
			decoded: func(t *testing.T, body []byte) {
				status, err := db2contract.DecodeOntologyEvalStatusReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if status != "" {
					t.Fatalf("status = %q for a relation nobody proposed", status)
				}
			},
		},
		{
			name:  "global_constraints",
			stage: db2contract.StageGlobalConstraints,
			encode: func() ([]byte, error) {
				return db2contract.EncodeGlobalConstraintsRequest(liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeGlobalConstraintsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kv_section",
			stage: db2contract.StageKvSection,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKvSectionRequest(kvSectionActiveTasks, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeKvSectionReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "recall_section",
			stage: db2contract.StageRecallSection,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRecallSectionRequest(recallSectionIdentity, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRecallSectionReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_candidates",
			stage: db2contract.StageMemoryCandidates,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryCandidatesRequest(memoryCandidatesPrimary, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryCandidatesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "briefing_active_entities",
			stage: db2contract.StageBriefingActiveEntities,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBriefingActiveEntitiesRequest(8, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeBriefingActiveEntitiesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "briefing_key_facts",
			stage: db2contract.StageBriefingKeyFacts,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBriefingKeyFactsRequest(liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeBriefingKeyFactsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "briefing_recent_activity",
			stage: db2contract.StageBriefingRecentActivity,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBriefingRecentActivityRequest(liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeBriefingRecentActivityReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_key_facts_provenance",
			stage: db2contract.StageMemoryKeyFactsProvenance,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryKeyFactsProvenanceRequest(liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryKeyFactsProvenanceReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "lifecycle_stale_pending",
			stage: db2contract.StageLifecycleStalePending,
			encode: func() ([]byte, error) {
				return db2contract.EncodeLifecycleStalePendingRequest(liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeLifecycleStalePendingReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "lifecycle_newly_superseded",
			stage: db2contract.StageLifecycleNewlySuperseded,
			encode: func() ([]byte, error) {
				return db2contract.EncodeLifecycleNewlySupersededRequest("", liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeLifecycleNewlySupersededReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_search_by_pattern",
			stage: db2contract.StageMemorySearchByPattern,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySearchByPatternRequest("%live-probe%", liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySearchByPatternReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_episodes_search",
			stage: db2contract.StageMemoryEpisodesSearch,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryEpisodesSearchRequest("live-probe", 8, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryEpisodesSearchReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "relations_for_entity",
			stage: db2contract.StageRelationsForEntity,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRelationsForEntityRequest("live-probe", 8, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRelationsForEntityReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "relations_search",
			stage: db2contract.StageRelationsSearch,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRelationsSearchRequest("live-probe", 8, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRelationsSearchReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "relations_search_as_of",
			stage: db2contract.StageRelationsSearchAsOf,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRelationsSearchAsOfRequest("live-probe",
					"2026-01-01 00:00:00", 8, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRelationsSearchAsOfReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "relations_supporting",
			stage: db2contract.StageRelationsSupporting,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRelationsSupportingRequest("live-probe", 8, liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRelationsSupportingReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "bandit_arms_list",
			stage: db2contract.StageBanditArmsList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditArmsListRequest("live-probe-point")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeBanditArmsListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "bandit_promotion_get",
			stage: db2contract.StageBanditPromotionGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditPromotionGetRequest("live-probe-point")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeBanditPromotionGetReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "calibration_surfaces_with_data",
			stage: db2contract.StageCalibrationSurfacesWithData,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCalibrationSurfacesWithDataRequest(3)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCalibrationSurfacesWithDataReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "artifact_target_surface",
			stage: db2contract.StageArtifactTargetSurface,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactTargetSurfaceRequest("live-probe-artifact")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeArtifactTargetSurfaceReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "audit_latest_before",
			stage: db2contract.StageAuditLatestBefore,
			encode: func() ([]byte, error) {
				return db2contract.EncodeAuditLatestBeforeRequest("live-probe-artifact")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeAuditLatestBeforeReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "evidence_pending_list",
			stage: db2contract.StageEvidencePendingList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEvidencePendingListRequest()
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEvidencePendingListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "unique_file_basename",
			stage: db2contract.StageUniqueFileBasename,
			// A basename nothing carries, so the answer is empty for the right
			// reason. What this proves is that regexp_replace and the
			// generation join resolve against the real schema; a fake cannot
			// run either.
			encode: func() ([]byte, error) {
				return db2contract.EncodeUniqueFileBasenameRequest(
					"replay-project", "live-probe-no-such-file.c")
			},
			decoded: func(t *testing.T, body []byte) {
				path, err := db2contract.DecodeUniqueFileBasenameReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if path != "" {
					t.Fatalf("path = %q for a basename nothing carries", path)
				}
			},
		},
		{
			name:  "verdict_suppressed",
			stage: db2contract.StageVerdictSuppressed,
			encode: func() ([]byte, error) {
				return db2contract.EncodeVerdictSuppressedRequest(
					"live-probe-tag", "live-probe-scope")
			},
			decoded: func(t *testing.T, body []byte) {
				suppressed, err := db2contract.DecodeVerdictSuppressedReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if suppressed != 0 {
					t.Fatal("a tag nobody has refused reads as suppressed")
				}
			},
		},
		{
			name:  "project_stats",
			stage: db2contract.StageProjectStats,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectStatsRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, err := db2contract.DecodeProjectStatsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "proposals_settled_counts",
			stage: db2contract.StageProposalsSettledCounts,
			// The window fallback and pg_now_text both have to execute, and SUM
			// over an empty set answers NULL here -- which is exactly the shape
			// a plain integer scan would fail on.
			encode: func() ([]byte, error) {
				return db2contract.EncodeProposalsSettledCountsRequest(30)
			},
			decoded: func(t *testing.T, body []byte) {
				committed, terminal, err := db2contract.DecodeProposalsSettledCountsReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if terminal < committed {
					t.Fatalf("terminal = %d is below committed = %d; terminal is "+
						"committed plus archived and cannot be smaller",
						terminal, committed)
				}
			},
		},
		{
			name:  "retrieval_event_by_turn",
			stage: db2contract.StageRetrievalEventByTurn,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRetrievalEventByTurnRequest("live-probe-turn")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, err := db2contract.DecodeRetrievalEventByTurnReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "artifact_links_read",
			stage: db2contract.StageArtifactLinksRead,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactLinksReadRequest("live-probe-artifact")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeArtifactLinksReadReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "corpus_pipeline_stage_counts",
			stage:  db2contract.StageCorpusPipelineStageCounts,
			encode: db2contract.EncodeCorpusPipelineStageCountsRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCorpusPipelineStageCountsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "scene_member_exists",
			stage: db2contract.StageSceneMemberExists,
			encode: func() ([]byte, error) {
				return db2contract.EncodeSceneMemberExistsRequest(2147483000, 2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeSceneMemberExistsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "unit_edge_exists",
			stage: db2contract.StageUnitEdgeExists,
			encode: func() ([]byte, error) {
				return db2contract.EncodeUnitEdgeExistsRequest(2147483000, 2147483001)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeUnitEdgeExistsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memories_by_key",
			stage: db2contract.StageMemoriesByKey,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoriesByKeyRequest("live-probe-no-such-key")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoriesByKeyReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_scopes_list",
			stage: db2contract.StageMemoryScopesList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryScopesListRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryScopesListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_scene_memberships",
			stage: db2contract.StageMemorySceneMemberships,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySceneMembershipsRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySceneMembershipsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_tier_kind_counts",
			stage: db2contract.StageMemoryTierKindCounts,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryTierKindCountsRequest()
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryTierKindCountsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_confidence_by_key",
			stage: db2contract.StageMemoryConfidenceByKey,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryConfidenceByKeyRequest("live-probe-no-such-key")
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, err := db2contract.DecodeMemoryConfidenceByKeyReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				// Found is what separates a memory nobody believes from a key
				// nothing holds. Against this schema it must be the second.
				if found != 0 {
					t.Fatal("a key nothing holds reported as found")
				}
			},
		},
		{
			name:  "memory_salience",
			stage: db2contract.StageMemorySalience,
			// A memory nothing holds, so the reply must be the default that was
			// sent -- which is what proves the default covers absence rather
			// than being discarded.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySalienceRequest(2147483000, 0.25)
			},
			decoded: func(t *testing.T, body []byte) {
				score, err := db2contract.DecodeMemorySalienceReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if score != 0.25 {
					t.Fatalf("salience = %v for an absent memory, want the default", score)
				}
			},
		},
		{
			name:  "memory_surprise",
			stage: db2contract.StageMemorySurprise,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySurpriseRequest(2147483000, 0.25)
			},
			decoded: func(t *testing.T, body []byte) {
				score, err := db2contract.DecodeMemorySurpriseReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if score != 0.25 {
					t.Fatalf("surprise = %v for an absent memory, want the default", score)
				}
			},
		},
		{
			name:  "ontology_eval_count",
			stage: db2contract.StageOntologyEvalCount,
			encode: func() ([]byte, error) {
				return db2contract.EncodeOntologyEvalCountRequest("live-probe-never-proposed")
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, err := db2contract.DecodeOntologyEvalCountReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a relation nobody proposed reported as found")
				}
			},
		},
		{
			name:  "decision_log_active_id",
			stage: db2contract.StageDecisionLogActiveID,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogActiveIDRequest("live-probe-subject", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDecisionLogActiveIDReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "document_stored_hash",
			stage: db2contract.StageDocumentStoredHash,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDocumentStoredHashRequest(
					"replay-project", "live-probe-no-such-file.md")
			},
			decoded: func(t *testing.T, body []byte) {
				hash, err := db2contract.DecodeDocumentStoredHashReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if hash != "" {
					t.Fatalf("hash = %q for a file nothing has ingested", hash)
				}
			},
		},
		{
			name:  "document_chunk_ids",
			stage: db2contract.StageDocumentChunkIds,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDocumentChunkIdsRequest(
					"replay-project", "live-probe-no-such-file.md")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDocumentChunkIdsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "pdf_tsr_state",
			stage: db2contract.StagePdfTsrState,
			encode: func() ([]byte, error) {
				return db2contract.EncodePdfTsrStateRequest(
					"replay-project", "live-probe-no-such-file.pdf")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodePdfTsrStateReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "retryable_index_failures",
			stage: db2contract.StageRetryableIndexFailures,
			// This is the probe that matters most in the batch. The previous
			// statement was invalid SQL -- PostgreSQL refuses to order a
			// DISTINCT by an unselected column -- so it never ran at all, and
			// only a real database can tell the difference between that and an
			// empty queue.
			encode: func() ([]byte, error) {
				return db2contract.EncodeRetryableIndexFailuresRequest(3, 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRetryableIndexFailuresReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "witness_checkpoint_freshness",
			stage:  db2contract.StageWitnessCheckpointFreshness,
			encode: db2contract.EncodeWitnessCheckpointFreshnessRequest,
			decoded: func(t *testing.T, body []byte) {
				read, count, _, err := db2contract.DecodeWitnessCheckpointFreshnessReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if read != 1 {
					t.Fatal("the read did not report itself as having happened")
				}
				_ = count
			},
		},
		{
			name:  "bandit_explore_stats",
			stage: db2contract.StageBanditExploreStats,
			// A real window, so make_interval and the ISO-8601 cutoff both have
			// to execute rather than being skipped by the zero branch.
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditExploreStatsRequest("live-probe-point", 3600)
			},
			decoded: func(t *testing.T, body []byte) {
				explore, total, err := db2contract.DecodeBanditExploreStatsReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if explore > total {
					t.Fatalf("explore = %d exceeds total = %d; the columns are crossed",
						explore, total)
				}
			},
		},
	}
}

// rollbackStore runs an operation inside a transaction the test always rolls
// back, so a write can be proven to parse and run without leaving anything in a
// database other tests share.
//
// Its InTx runs fn directly rather than opening a nested one. The operation's
// own boundary is subsumed by the outer transaction, which is sound here only
// because the outer one never commits: what is proven is that the statements
// run, not that the operation's commit semantics hold. Those are the fakes'
// job, and TestDecisionLogRecordRollsBackWhenTheSupersedeMissed is where they
// are checked.
type rollbackStore struct {
	tx pgx.Tx
}

func (s *rollbackStore) Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error) {
	return s.tx.Query(ctx, sql, args...)
}

func (s *rollbackStore) QueryRow(ctx context.Context, sql string, args ...any) pgx.Row {
	return s.tx.QueryRow(ctx, sql, args...)
}

func (s *rollbackStore) Exec(ctx context.Context, sql string, args ...any) (int64, error) {
	tag, err := s.tx.Exec(ctx, sql, args...)
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

func (s *rollbackStore) InTx(ctx context.Context, fn func(Store) error) error {
	return fn(s)
}

func liveWrites() []liveRequest {
	return []liveRequest{
		{
			name:  "prospective_insert",
			stage: db2contract.StageProspectiveInsert,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveInsertRequest(
					"live trigger", "live action", "", "", "once", "", "live-probe")
			},
			decoded: func(t *testing.T, body []byte) {
				id, err := db2contract.DecodeProspectiveInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if id == 0 {
					t.Fatal("the insert returned no identifier")
				}
			},
		},
		{
			name:  "directive_insert_ignore",
			stage: db2contract.StageDirectiveInsertIgnore,
			// Twice: the first raises the directive, the second hits
			// idx_directives_dedup_topic and takes the conflict path. Both
			// halves of the operation run, which is the only way to prove the
			// natural-key lookup is a statement.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveInsertIgnoreRequest(
					"live probe question?", "live-probe-topic", "", "",
					"retrieval_failure", 5, 0, 0, "", "live-probe", "")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, _, _, err := db2contract.DecodeDirectiveInsertIgnoreReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the directive was neither raised nor found")
				}
			},
		},
		{
			name:  "file_index_delete_current_generation",
			stage: db2contract.StageFileIndexDeleteCurrentGeneration,
			encode: func() ([]byte, error) {
				return db2contract.EncodeFileIndexDeleteCurrentGenerationRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				deleted, err := db2contract.DecodeFileIndexDeleteCurrentGenerationReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if deleted != 1 {
					t.Fatal("the delete did not run")
				}
			},
		},
		{
			name:  "minhash_delete_current_generation",
			stage: db2contract.StageMinhashDeleteCurrentGeneration,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMinhashDeleteCurrentGenerationRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMinhashDeleteCurrentGenerationReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("both deletes did not run")
				}
			},
		},
		{
			name:  "decision_log_insert",
			stage: db2contract.StageDecisionLogInsert,
			// This probe used to have to retire the occupied ('', 0) slot
			// before it could insert, because idx_dl_active_scope covered
			// unscoped rows and admitted exactly one. That was a defect in the
			// index rather than in the probe -- the invariant belongs to the
			// governance write, which cannot produce an unscoped row -- and the
			// predicate now says `subject <> ''`. Two plain decisions logged
			// against the same database is the case that was broken; the second
			// insert below is that case.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogInsertRequest(
					0, "live options", "live chosen", "", "", "")
			},
			// Run twice, because once proves nothing here: the defect this
			// covers let the first unscoped insert through and refused every
			// one after it.
			repeat: 2,
			decoded: func(t *testing.T, body []byte) {
				acknowledged, id, _, _, _, _, _, _, createdAt, status, _, _, _, _, _, err :=
					db2contract.DecodeDecisionLogInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 || id == 0 {
					t.Fatalf("acknowledged = %d, id = %d", acknowledged, id)
				}
				// The database stamped it and defaulted the status: both are
				// columns the caller never sent, which is what proves the row
				// came back rather than the request.
				if createdAt == "" || status != "active" {
					t.Fatalf("createdAt = %q, status = %q", createdAt, status)
				}
			},
		},
		{
			name:  "decision_log_record",
			stage: db2contract.StageDecisionLogRecord,
			encode: func() ([]byte, error) {
				// Superseding nothing, so the insert stands alone: the supersede
				// path needs an active decision to aim at, which a probe leaving
				// nothing behind cannot arrange.
				return db2contract.EncodeDecisionLogRecordRequest(
					"live-subject", "live options", "live chosen", "", "live-probe", 0, "", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, id, _, _, _, _, _, _, _, status, _, _, subject, _, _, err :=
					db2contract.DecodeDecisionLogRecordReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 || id == 0 || status != "active" ||
					subject != "live-subject" {
					t.Fatalf("acknowledged=%d id=%d status=%q subject=%q",
						acknowledged, id, status, subject)
				}
			},
		},
		{
			name:  "projection_generation_create",
			stage: db2contract.StageProjectionGenerationCreate,
			seed:  []string{liveProbeProject},
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectionGenerationCreateRequest(liveProbeProjectName)
			},
			decoded: func(t *testing.T, body []byte) {
				generation, err := db2contract.DecodeProjectionGenerationCreateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				// Non-zero is the whole point: the INSERT ... SELECT only
				// inserts when the project is current, and a seeded current
				// project must therefore get an identifier back.
				if generation == 0 {
					t.Fatal("no generation was opened for a current project")
				}
			},
		},
		{
			name:  "generation_publish",
			stage: db2contract.StageGenerationPublish,
			// A visible generation as well as the pending one, so the supersede
			// has something to retire. Publishing into an empty table would run
			// all three statements and prove nothing about the ordering that
			// protects the last visible graph.
			seed: []string{
				liveProbeProject,
				liveProbeVisibleGeneration,
				liveProbePendingGeneration,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeGenerationPublishRequest(
					liveProbePendingGenerationID, liveProbeProjectName)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeGenerationPublishReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a pending generation on a current project did not publish")
				}
			},
		},
		{
			name:  "generation_abort",
			stage: db2contract.StageGenerationAbort,
			seed:  []string{liveProbeProject, liveProbePendingGeneration},
			encode: func() ([]byte, error) {
				return db2contract.EncodeGenerationAbortRequest(
					liveProbePendingGenerationID, "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeGenerationAbortReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the abort did not run")
				}
			},
		},
		{
			name:  "generation_set_source_hash",
			stage: db2contract.StageGenerationSetSourceHash,
			seed:  []string{liveProbeProject, liveProbePendingGeneration},
			encode: func() ([]byte, error) {
				return db2contract.EncodeGenerationSetSourceHashRequest(
					liveProbePendingGenerationID, "live-probe-source-hash")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeGenerationSetSourceHashReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "project_delete",
			stage: db2contract.StageProjectDelete,
			// Seeded with the generation rows too, so the delete runs against a
			// project something references. If a cascade rule were missing the
			// statement would fail here rather than in production.
			seed: []string{
				liveProbeProject,
				liveProbeVisibleGeneration,
				liveProbePendingGeneration,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectDeleteRequest(liveProbeProjectName)
			},
			decoded: func(t *testing.T, body []byte) {
				deleted, err := db2contract.DecodeProjectDeleteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if deleted != 1 {
					t.Fatal("the delete did not run")
				}
			},
		},
		{
			name:  "collab_rule_approve",
			stage: db2contract.StageCollabRuleApprove,
			// The interesting statement is the one with the cap subquery in its
			// WHERE, which a fake cannot execute. Seeded with a proposed rule so
			// the transition matches, and the epoch bump then has to run too --
			// including its ON CONFLICT upsert against a TEXT column it casts
			// through INTEGER.
			seed: []string{liveProbeProposedRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCollabRuleApproveRequest(liveProbeRuleID)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeCollabRuleApproveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a proposed rule under the active cap did not approve")
				}
			},
		},
		{
			name:  "collab_rule_reject",
			stage: db2contract.StageCollabRuleReject,
			seed:  []string{liveProbeProposedRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCollabRuleRejectRequest(liveProbeRuleID)
			},
			decoded: func(t *testing.T, body []byte) {
				changed, err := db2contract.DecodeCollabRuleRejectReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if changed != 1 {
					t.Fatal("a proposed rule did not reject")
				}
			},
		},
		{
			name:  "collab_rule_retire",
			stage: db2contract.StageCollabRuleRetire,
			// Active rather than proposed: retire moves from a different state
			// than its two neighbours, so seeding it the same way would probe a
			// transition that always misses.
			seed: []string{liveProbeActiveRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCollabRuleRetireRequest(liveProbeRuleID)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeCollabRuleRetireReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an active rule did not retire")
				}
			},
		},
		{
			name:  "proposal_mark_committed",
			stage: db2contract.StageProposalMarkCommitted,
			// Twice, because being repeatable is the behaviour: it has no state
			// predicate, so the second call re-commits an already-committed
			// proposal and must still acknowledge.
			repeat: 2,
			seed:   []string{liveProbeProposal},
			encode: func() ([]byte, error) {
				return db2contract.EncodeProposalMarkCommittedRequest(liveProbeProposalID)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeProposalMarkCommittedReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("mark_committed did not acknowledge")
				}
			},
		},
		{
			name:   "proposal_bump_corroboration",
			stage:  db2contract.StageProposalBumpCorroboration,
			seed:   []string{liveProbeProposal},
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProposalBumpCorroborationRequest(liveProbeProposalID)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeProposalBumpCorroborationReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the bump did not acknowledge")
				}
			},
		},
		{
			name:  "rules_delete_by_id",
			stage: db2contract.StageRulesDeleteByID,
			seed:  []string{liveProbeRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesDeleteByIDRequest(liveProbeRuleID)
			},
			decoded: func(t *testing.T, body []byte) {
				deleted, err := db2contract.DecodeRulesDeleteByIDReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if deleted != 1 {
					t.Fatal("a seeded rule was not removed")
				}
			},
		},
		{
			name:  "rules_delete_by_directive_type",
			stage: db2contract.StageRulesDeleteByDirectiveType,
			seed:  []string{liveProbeRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesDeleteByDirectiveTypeRequest("hard")
			},
			decoded: func(t *testing.T, body []byte) {
				deleted, err := db2contract.DecodeRulesDeleteByDirectiveTypeReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				// A count, not a flag. One row is seeded, so one is the answer;
				// a port that returned a flag would also pass this, which is why
				// the fake test covers the many-row case.
				if deleted != 1 {
					t.Fatalf("deleted = %d for one seeded rule", deleted)
				}
			},
		},
		{
			name:  "vector_index_op_remove",
			stage: db2contract.StageVectorIndexOpRemove,
			encode: func() ([]byte, error) {
				return db2contract.EncodeVectorIndexOpRemoveRequest(900004)
			},
			decoded: func(t *testing.T, body []byte) {
				recorded, err := db2contract.DecodeVectorIndexOpRemoveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if recorded != 1 {
					t.Fatal("the delete did not run")
				}
			},
		},
		{
			name:  "reset_stuck_vector_ops",
			stage: db2contract.StageResetStuckVectorOps,
			// Both queues have to exist for this to answer: the operation
			// touches vector_index_ops and code_index_ops, and a port that
			// reset only the first would pass a fake and fail here only if the
			// second table were missing. It is here mainly to prove both
			// statements parse against the real schema.
			encode: func() ([]byte, error) {
				return db2contract.EncodeResetStuckVectorOpsRequest(3)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeResetStuckVectorOpsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_release_promote",
			stage: db2contract.StageKBReleasePromote,
			// A release and an active pointer naming a different one, so the
			// retire, the promote and the pointer upsert all match something.
			seed: []string{
				liveProbeRetiredRelease,
				liveProbeRelease,
				liveProbeActivePointer,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBReleasePromoteRequest(liveProbeReleaseID)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBReleasePromoteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an existing release did not promote")
				}
			},
		},
		{
			name:  "kb_release_rollback",
			stage: db2contract.StageKBReleaseRollback,
			// Zero, which is the branch that resolves a target for itself from
			// the most recently retired release. Passing an identifier would
			// probe the same code path as promote and prove nothing new.
			seed: []string{
				liveProbeRetiredRelease,
				liveProbeRelease,
				liveProbeActivePointer,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBReleaseRollbackRequest(0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBReleaseRollbackReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a retired release was not rolled back to")
				}
			},
		},
		{
			name:  "memory_retro_scan_marker",
			stage: db2contract.StageMemoryRetroScanMarker,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryRetroScanMarkerRequest("2026-01-01T00:00:00Z")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryRetroScanMarkerReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the marker was not written")
				}
			},
		},
		{
			name:  "dedupe_by_key",
			stage: db2contract.StageDedupeByKey,
			// Two memories sharing a key, so the merge half actually matches
			// something: the UPDATE ... FROM, the RETURNING and the provenance
			// INSERT that reads from it all have to run. Against an empty
			// schema the statement would parse and merge nothing, which is the
			// same reply a broken join gives.
			seed: []string{liveProbeDuplicateMemories},
			encode: func() ([]byte, error) {
				return db2contract.EncodeDedupeByKeyRequest(0)
			},
			decoded: func(t *testing.T, body []byte) {
				merged, err := db2contract.DecodeDedupeByKeyReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if merged < 1 {
					t.Fatal("two memories sharing a key did not merge")
				}
			},
		},
		{
			name:  "enrollment_touch_last_seen",
			stage: db2contract.StageEnrollmentTouchLastSeen,
			// The insert half: nothing holds this fingerprint, so the backfill
			// path runs and the minted authority id has to satisfy the column.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEnrollmentTouchLastSeenRequest(
					"live-probe-fingerprint", "kb")
			},
			decoded: func(t *testing.T, body []byte) {
				recorded, err := db2contract.DecodeEnrollmentTouchLastSeenReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if recorded != 1 {
					t.Fatal("the sighting was not recorded")
				}
			},
		},
		{
			name:  "ontology_approve",
			stage: db2contract.StageOntologyApprove,
			// An open evaluation and the rel_type it decides, so both halves
			// match something. Against an empty schema the evaluation update
			// matches nothing and the operation refuses, which would probe the
			// refusal rather than the decision.
			seed: []string{liveProbeRelType, liveProbeEvaluation},
			encode: func() ([]byte, error) {
				return db2contract.EncodeOntologyApproveRequest(liveProbeRelTypeName)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeOntologyApproveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an open evaluation was not approved")
				}
			},
		},
		{
			name:  "ontology_reject",
			stage: db2contract.StageOntologyReject,
			seed:  []string{liveProbeRelType, liveProbeEvaluation},
			encode: func() ([]byte, error) {
				return db2contract.EncodeOntologyRejectRequest(liveProbeRelTypeName)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeOntologyRejectReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an open evaluation was not rejected")
				}
			},
		},
		{
			name:  "release_create",
			stage: db2contract.StageReleaseCreate,
			encode: func() ([]byte, error) {
				return db2contract.EncodeReleaseCreateRequest("live-probe-new-release")
			},
			decoded: func(t *testing.T, body []byte) {
				id, err := db2contract.DecodeReleaseCreateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if id == 0 {
					t.Fatal("no release was created")
				}
			},
		},
		{
			name:  "decision_log_set_outcome",
			stage: db2contract.StageDecisionLogSetOutcome,
			// Seeded, because these three refuse when nothing changed: against
			// an empty schema every one of them would probe the refusal rather
			// than the write.
			seed: []string{liveProbeDecision},
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogSetOutcomeRequest(
					liveProbeDecisionID, "live probe outcome")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeDecisionLogSetOutcomeReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a decision that exists did not take its outcome")
				}
			},
		},
		{
			name:  "decision_log_set_status",
			stage: db2contract.StageDecisionLogSetStatus,
			seed:  []string{liveProbeDecision},
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogSetStatusRequest(
					liveProbeDecisionID, "retired")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeDecisionLogSetStatusReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a decision that exists did not take its status")
				}
			},
		},
		{
			name:  "decision_log_set_revisit",
			stage: db2contract.StageDecisionLogSetRevisit,
			seed:  []string{liveProbeDecision},
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogSetRevisitRequest(
					liveProbeDecisionID, "2026-06-01")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeDecisionLogSetRevisitReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a decision that exists did not take its revisit date")
				}
			},
		},
		{
			name:  "proposal_archive",
			stage: db2contract.StageProposalArchive,
			// Twice: repeatability is the behaviour, and the second call
			// archives an already-archived proposal.
			repeat: 2,
			seed:   []string{liveProbeProposal},
			encode: func() ([]byte, error) {
				return db2contract.EncodeProposalArchiveRequest(
					liveProbeProposalID, "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeProposalArchiveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the archive did not acknowledge")
				}
			},
		},
		{
			name:  "rules_update_directive_type",
			stage: db2contract.StageRulesUpdateDirectiveType,
			seed:  []string{liveProbeRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesUpdateDirectiveTypeRequest(
					liveProbeRuleID, "soft")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeRulesUpdateDirectiveTypeReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "bandit_decision_close",
			stage: db2contract.StageBanditDecisionClose,
			// Seeded so the UPDATE matches, which is what exercises the
			// DOUBLE PRECISION bind and the timestamp expression. It would
			// acknowledge either way -- the reply reports the statement, not a
			// row -- so an unseeded probe would prove neither.
			seed: []string{liveProbeBanditDecision},
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditDecisionCloseRequest(
					liveProbeBanditDecisionID, 0.75)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeBanditDecisionCloseReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the close did not run")
				}
			},
		},
		{
			name:  "minhash_delete_file",
			stage: db2contract.StageMinhashDeleteFile,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMinhashDeleteFileRequest(
					"replay-project", "src/live-probe.c")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMinhashDeleteFileReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("both deletes did not run")
				}
			},
		},
		{
			name:  "entity_edge_bump_utility",
			stage: db2contract.StageEntityEdgeBumpUtility,
			// The clamp is a SQL expression over a DOUBLE PRECISION column, so
			// it has to be executed rather than inspected.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityEdgeBumpUtilityRequest("live-probe-entity", 0.5)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeEntityEdgeBumpUtilityReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the bump did not run")
				}
			},
		},
		{
			name:  "artifact_flag_review",
			stage: db2contract.StageArtifactFlagReview,
			// Seeded because this one refuses when nothing changed, and because
			// the jsonb merge is the point: it has to run against a real JSONB
			// column with a real object in it.
			seed: []string{liveProbeArtifact},
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactFlagReviewRequest(
					liveProbeArtifactID, "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeArtifactFlagReviewReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an artifact that exists was not flagged")
				}
			},
		},
		{
			name:  "purge_files_matching",
			stage: db2contract.StagePurgeFilesMatching,
			// A pattern nothing matches, against a project id nothing holds.
			// The generation subquery still has to resolve, which is the half a
			// fake cannot run.
			encode: func() ([]byte, error) {
				return db2contract.EncodePurgeFilesMatchingRequest(
					2147483000, "live-probe-no-such-path/%")
			},
			decoded: func(t *testing.T, body []byte) {
				deleted, err := db2contract.DecodePurgeFilesMatchingReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if deleted != 0 {
					t.Fatalf("deleted = %d for a project nothing holds", deleted)
				}
			},
		},
		{
			name:  "prospective_set_state",
			stage: db2contract.StageProspectiveSetState,
			// Acknowledges whether or not a row matched, so an unseeded probe
			// still proves the statement runs -- which is the half a fake
			// cannot show.
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveSetStateRequest(2147483000, "cancelled")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeProspectiveSetStateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "lifecycle_mark_pending",
			stage: db2contract.StageLifecycleMarkPending,
			// pg_now_text has to accept the bound interval, which is the whole
			// reason the day count moved out of the statement text.
			encode: func() ([]byte, error) {
				return db2contract.EncodeLifecycleMarkPendingRequest(2147483000, 14)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeLifecycleMarkPendingReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run; the bound interval may not parse")
				}
			},
		},
		{
			name:  "directive_resolve",
			stage: db2contract.StageDirectiveResolve,
			// Seeded because this one refuses when nothing changed: unseeded it
			// would probe the refusal rather than the resolve.
			seed: []string{liveProbeDirective},
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveResolveRequest(liveProbeDirectiveID, 1)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeDirectiveResolveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an open directive was not resolved")
				}
			},
		},
		{
			name:  "doc_assets_delete_for_doc",
			stage: db2contract.StageDocAssetsDeleteForDoc,
			// The nested generation subqueries and the document check all have
			// to resolve, which is the half a fake cannot run.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDocAssetsDeleteForDocRequest(
					"replay-project", "live-probe-no-such-file.pdf")
			},
			decoded: func(t *testing.T, body []byte) {
				deleted, err := db2contract.DecodeDocAssetsDeleteForDocReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if deleted != 0 {
					t.Fatalf("deleted = %d for a document nothing holds", deleted)
				}
			},
		},
		{
			name:  "release_add_doc",
			stage: db2contract.StageReleaseAddDoc,
			// Seeded because release_docs carries foreign keys into both
			// doc_releases and docs; an unseeded insert fails on them rather
			// than on anything this operation decides.
			//
			// Twice, because being repeatable is the behaviour: the second call
			// takes the ON CONFLICT path.
			repeat: 2,
			seed: []string{
				liveProbeRelease,
				liveProbeDoc,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeReleaseAddDocRequest(
					liveProbeReleaseID, liveProbeDocID)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeReleaseAddDocReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the insert did not run")
				}
			},
		},
		{
			name:  "ontology_map",
			stage: db2contract.StageOntologyMap,
			// A target that exists and an open evaluation for the source, so
			// all three checks pass and both tables move.
			seed: []string{
				liveProbeRelType,
				liveProbeEvaluation,
				liveProbeMapTarget,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeOntologyMapRequest(
					liveProbeRelTypeName, liveProbeMapTargetName)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeOntologyMapReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a relation with an open evaluation and a real target " +
						"was not mapped")
				}
			},
		},
		{
			name:  "ingest_queue_fail",
			stage: db2contract.StageIngestQueueFail,
			encode: func() ([]byte, error) {
				return db2contract.EncodeIngestQueueFailRequest(2147483000, "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeIngestQueueFailReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "kb_documents_delete_for_file",
			stage: db2contract.StageKBDocumentsDeleteForFile,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocumentsDeleteForFileRequest(
					"replay-project", "live-probe-no-such-file.md")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBDocumentsDeleteForFileReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the delete did not run")
				}
			},
		},
		{
			name:  "kb_documents_link_neighbours",
			stage: db2contract.StageKBDocumentsLinkNeighbours,
			// Neither statement has to match a row -- a chunk since deleted
			// links to nothing -- so an unseeded probe still exercises both
			// updates and the transaction around them.
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocumentsLinkNeighboursRequest(
					2147483001, 2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBDocumentsLinkNeighboursReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("both updates did not run")
				}
			},
		},
		{
			name:  "bandit_arm_stats_update",
			stage: db2contract.StageBanditArmStatsUpdate,
			// The probe that matters here. The C form of this statement was
			// rejected by PostgreSQL every time it ran -- an untyped product --
			// and only a real server can tell that apart from a write that
			// landed. Run twice so the ON CONFLICT branch executes too, which
			// carries the same product.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditArmStatsUpdateRequest(
					"live-probe-point", "live-probe-arm", 0.5, 2.0, 3.0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeBanditArmStatsUpdateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the arm statistics were not written")
				}
			},
		},
		{
			name:  "bandit_promotion_set",
			stage: db2contract.StageBanditPromotionSet,
			// Twice, so the conflict path replaces rather than inserts.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditPromotionSetRequest(
					"live-probe-point", "live-probe-arm", "live-probe-rollback")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeBanditPromotionSetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the promotion was not written")
				}
			},
		},
	}
}

// Seed rows for the projection-lifecycle probes. The identifiers are fixed
// rather than generated because a probe has to name the generation it publishes
// and a seed statement cannot hand one back; they are high enough not to meet a
// real row, and the transaction rolls back regardless.
const (
	liveProbeProjectName         = "live-probe-project"
	liveProbePendingGenerationID = 900001
	liveProbeProject             = `INSERT INTO projects (name, root, scanned_at, lifecycle_state)
 VALUES ('live-probe-project', '/live-probe', '2026-01-01 00:00:00', 'current')`
	liveProbeVisibleGeneration = `INSERT INTO code_projection_generations
 (id, project, state, started_at) VALUES
 (900000, 'live-probe-project', 'visible', '2026-01-01 00:00:00')`
	liveProbePendingGeneration = `INSERT INTO code_projection_generations
 (id, project, state, started_at) VALUES
 (900001, 'live-probe-project', 'pending', '2026-01-01 00:00:00')`
)

// Seed rows for the learning-write probes. Fixed identifiers for the same
// reason as the projection ones: a probe has to name what it acts on.
const (
	liveProbeRuleID       = 900002
	liveProbeProposalID   = 900003
	liveProbeProposedRule = `INSERT INTO collab_rules (id, text, status, created_at)
 VALUES (900002, 'live probe rule', 'proposed', '2026-01-01 00:00:00')`
	liveProbeActiveRule = `INSERT INTO collab_rules (id, text, status, created_at)
 VALUES (900002, 'live probe rule', 'active', '2026-01-01 00:00:00')`
	liveProbeRule = `INSERT INTO rules
 (id, polarity, title, directive_type, created_at, updated_at)
 VALUES (900002, 'positive', 'live probe rule', 'hard',
 '2026-01-01 00:00:00', '2026-01-01 00:00:00')`
	// learning_proposals.signal_id is a foreign key, so the signal has to exist
	// before the proposal does. Both statements are in the same seed entry
	// because neither is useful without the other.
	liveProbeProposal = `WITH signal AS (
 INSERT INTO learning_signals (id, signal_type, created_at)
 VALUES (900003, 'live-probe', '2026-01-01 00:00:00') RETURNING id)
 INSERT INTO learning_proposals (id, signal_id, sink, state, created_at, updated_at)
 SELECT 900003, signal.id, 'rules', 'pending',
 '2026-01-01 00:00:00', '2026-01-01 00:00:00' FROM signal`
)

// Seed rows for the release probes. The rollback branch needs a retired release
// to find, and the promote branch needs an active pointer to retire, so both
// probes seed all three.
const (
	liveProbeReleaseID = 900005
	liveProbeRelease   = `INSERT INTO doc_releases (id, name, state, created_at)
 VALUES (900005, 'live-probe-release', 'draft', '2026-01-01 00:00:00')`
	liveProbeRetiredRelease = `INSERT INTO doc_releases
 (id, name, state, created_at, retired_at)
 VALUES (900006, 'live-probe-release-prior', 'retired',
 '2026-01-01 00:00:00', '2026-01-02 00:00:00')`
	liveProbeActivePointer = `INSERT INTO kb_runtime_state (state_key, state_value)
 VALUES ('active_release_id', '900006')
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`
)

// Two memories under one key, for the dedupe probe. Against an empty schema the
// merge statement parses and merges nothing, which is the same reply a broken
// join would give -- so the probe needs something to actually merge.
const liveProbeDuplicateMemories = `INSERT INTO memories
 (id, tier, kind, key, content, confidence, merged_into, created_at, updated_at)
 VALUES
 (900007, 'L2', 'fact', 'live-probe-duplicate-key', 'canonical', 0.9, 0,
  '2026-01-01 00:00:00', '2026-01-01 00:00:00'),
 (900008, 'L2', 'fact', 'live-probe-duplicate-key', 'duplicate', 0.5, 0,
  '2026-01-01 00:00:00', '2026-01-01 00:00:00')`

// An open evaluation and its relation type, for the ontology decision probes.
// The name is already normalized, because that is the form the tables hold and
// the probe is not testing the normalizer here.
const (
	liveProbeRelTypeName = "live_probe_relation"
	liveProbeRelType     = `INSERT INTO rel_types (rel_type, status)
 VALUES ('live_probe_relation', 'provisional')`
	liveProbeEvaluation = `INSERT INTO ontology_evaluations (rel_type, status)
 VALUES ('live_probe_relation', 'pending')`
)

// Seed rows for the learning-setter probes.
const (
	liveProbeDecisionID       = 900007
	liveProbeBanditDecisionID = "live-probe-decision"
	liveProbeDecision         = `INSERT INTO decision_log
 (id, options, chosen, status, created_at)
 VALUES (900007, 'live probe options', 'live probe chosen', 'active',
 '2026-01-01 00:00:00')`
	liveProbeBanditDecision = `INSERT INTO bandit_decisions
 (id, decision_point, arm_id, decided_at)
 VALUES ('live-probe-decision', 'live-probe-point', 'live-probe-arm',
 '2026-01-01 00:00:00')`
)

// An artifact carrying a payload with a key of its own, so the flag probe
// exercises a merge rather than a replace.
const (
	liveProbeArtifactID = "live-probe-artifact"
	liveProbeArtifact   = `INSERT INTO artifacts (id, kind, state, payload)
 VALUES ('live-probe-artifact', 'live-probe', 'committed',
 '{"kept": "value"}'::jsonb)`
)

// An open directive for the resolve probe.
const (
	liveProbeDirectiveID = 900008
	liveProbeDirective   = `INSERT INTO epistemic_directives
 (id, question, cause, state, created_at, updated_at)
 VALUES (900008, 'live probe question?', 'retrieval_failure', 'open',
 '2026-01-01 00:00:00', '2026-01-01 00:00:00')`
)

// Seed rows for the document probes: a target relation for the map, and a doc
// for the release to hold.
const (
	liveProbeMapTargetName = "live_probe_target_relation"
	liveProbeMapTarget     = `INSERT INTO rel_types (rel_type, status)
 VALUES ('live_probe_target_relation', 'active')`
	liveProbeDocID = 900009
	liveProbeDoc   = `INSERT INTO docs (id, content_hash, filename)
 VALUES (900009, 'live-probe-content-hash', 'live-probe.md')`
)

func TestLiveWritesRunAndLeaveNothingBehind(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()
	pool := store.(*PoolStore).pool

	// Sequences do not roll back. A transaction that inserts a row with a
	// defaulted identity consumes a value, and rolling the row away leaves the
	// sequence where the insert left it -- by design, because two transactions
	// must not be handed the same identity.
	//
	// That makes the name of this test a half-truth, and the half it misses has
	// bitten: the C replay promotes doc_releases id 1, and a probe that created
	// a release moved the sequence on so the replay's own release was no longer
	// id 1. Snapshotting every sequence and putting them back is the general
	// fix, and it is general on purpose -- most write probes insert something,
	// and a list of which ones would go stale on the next batch.
	restoreSequences := snapshotSequences(t, pool)
	defer restoreSequences()

	for _, testCase := range liveWrites() {
		t.Run(testCase.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			tx, err := pool.Begin(ctx)
			if err != nil {
				t.Fatalf("begin: %v", err)
			}
			// Always. The probe proves the statements run; it must not decide
			// what the database holds afterwards.
			defer func() { _ = tx.Rollback(ctx) }()

			for _, statement := range testCase.seed {
				if _, err := tx.Exec(ctx, statement); err != nil {
					t.Fatalf("seed: %v\n%s", err, statement)
				}
			}

			handler := NewDispatchHandler(&rollbackStore{tx: tx})
			request, err := testCase.encode()
			if err != nil {
				t.Fatalf("encode request: %v", err)
			}
			attempts := testCase.repeat
			if attempts < 1 {
				attempts = 1
			}
			for attempt := range attempts {
				body, status := handler(invocation(testCase.stage), request)
				if status != bus.ModuleStatusOK {
					t.Fatalf("attempt %d: status = %v -- the statement did not run",
						attempt, status)
				}
				testCase.decoded(t, body)
			}
		})
	}
}

// snapshotSequences records every sequence in the schema and hands back a
// function that puts them all back where they were.
//
// is_called is carried as well as last_value: a sequence that has never been
// used reports last_value as its start with is_called false, and restoring it
// as called would silently skip the first identity the schema ever issues.
func snapshotSequences(t *testing.T, pool *pgxpool.Pool) func() {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	type sequenceState struct {
		name      string
		lastValue int64
		isCalled  bool
	}
	rows, err := pool.Query(ctx, `SELECT schemaname || '.' || sequencename,
 COALESCE(last_value, start_value), last_value IS NOT NULL
 FROM pg_sequences WHERE schemaname = current_schema()`)
	if err != nil {
		t.Fatalf("read sequences: %v", err)
	}
	var states []sequenceState
	for rows.Next() {
		var state sequenceState
		if err := rows.Scan(&state.name, &state.lastValue, &state.isCalled); err != nil {
			rows.Close()
			t.Fatalf("scan sequence: %v", err)
		}
		states = append(states, state)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		t.Fatalf("read sequences: %v", err)
	}

	return func() {
		restoreCtx, restoreCancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer restoreCancel()
		for _, state := range states {
			if _, err := pool.Exec(restoreCtx, "SELECT setval($1, $2, $3)",
				state.name, state.lastValue, state.isCalled); err != nil {
				// Reported rather than fatal: the probes have already run and
				// their assertions are the point of the test. A sequence left
				// advanced is a real leak and has to be visible, but failing
				// here would hide whatever the probes actually found.
				t.Errorf("restore sequence %s: %v", state.name, err)
			}
		}
	}
}

func TestLiveOperationsRunAgainstARealSchema(t *testing.T) {
	store, close := liveStore(t)
	defer close()
	handler := NewDispatchHandler(store)

	for _, testCase := range liveReads() {
		t.Run(testCase.name, func(t *testing.T) {
			request, err := testCase.encode()
			if err != nil {
				t.Fatalf("encode request: %v", err)
			}
			body, status := handler(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v -- the statement did not run", status)
			}
			testCase.decoded(t, body)
		})
	}
}

// liveExcluded names an operation that cannot be probed read-only, and why.
//
// An exclusion is a debt, not a dispensation: it says this implementation has
// never been run against a real schema. Naming it keeps that visible, where an
// unexplained gap in the coverage count would not be.
var liveExcluded = map[string]string{}

// Every registered operation is either probed above or named as excluded. A
// port that adds an implementation and neither gets one nothing has ever run
// against a database -- the state the C side spent a whole replay harness to
// get out of, and the reason that harness's own docstring calls it "the only
// test that proves a DB2 statement parses and runs".
func TestLiveCoversEveryPortedOperation(t *testing.T) {
	// Keyed on the entry name, not on the stage. A stage is a family and is
	// shared by every operation in it -- the dispatch key is the (stage,
	// operation) pair, and only the encoded request carries the second half --
	// so counting stages would collapse fifty-odd operations into eight.
	//
	// Names rather than entries, because an operation can earn more than one
	// probe: dedupe_by_key has a read for its dry run and a write for its
	// merge, since the two take different paths. Both entries carry the same
	// name and count once; they sit under different parent tests, so the
	// subtest names do not collide.
	probed := map[string]bool{}
	for _, entry := range liveReads() {
		probed[entry.name] = true
	}
	for _, entry := range liveWrites() {
		probed[entry.name] = true
	}
	covered := len(probed) + len(liveExcluded)
	if covered != Implemented() {
		t.Fatalf("%d operation(s) ported; %d distinct operation(s) probed, %d excluded "+
			"(%d read entries, %d write entries)",
			Implemented(), len(probed), len(liveExcluded),
			len(liveReads()), len(liveWrites()))
	}
}
