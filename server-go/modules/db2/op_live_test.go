package db2

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"sync"
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

// liveProbeEmbedding builds a vector of the width evidence_vectors.embedding
// declares. The dimension is part of the column type, so a literal of any other
// width is rejected -- which is exactly what a fake cannot tell you.
func liveProbeEmbedding() string {
	return "[" + strings.TrimSuffix(strings.Repeat("0,", 384), ",") + "]"
}

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
		{
			name:  "entity_neighbors",
			stage: db2contract.StageEntityNeighbors,
			// The projection-visibility subquery joins two tables and the
			// aggregating forms group over a union; neither is something a fake
			// can execute.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityNeighborsRequest("live-probe-entity", 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityNeighborsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_outbound_neighbors",
			stage: db2contract.StageEntityOutboundNeighbors,
			// The projection-visibility subquery joins two tables and the
			// aggregating forms group over a union; neither is something a fake
			// can execute.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityOutboundNeighborsRequest("live-probe-entity", 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityOutboundNeighborsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_top_targets",
			stage: db2contract.StageEntityTopTargets,
			// The projection-visibility subquery joins two tables and the
			// aggregating forms group over a union; neither is something a fake
			// can execute.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityTopTargetsRequest("live-probe-entity", "depends_on")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityTopTargetsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_top_partners",
			stage: db2contract.StageEntityTopPartners,
			// The projection-visibility subquery joins two tables and the
			// aggregating forms group over a union; neither is something a fake
			// can execute.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityTopPartnersRequest("live-probe-entity", "depends_on")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityTopPartnersReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "demotion_profile_read",
			stage: db2contract.StageDemotionProfileRead,
			// Three lookups run against a schema holding no profile, so all
			// three fall through -- and the CTE that stamps what it finds has
			// to plan even when it finds nothing.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDemotionProfileReadRequest(
					"live-probe-class", "project", "live-probe-scope")
			},
			decoded: func(t *testing.T, body []byte) {
				profile, err := db2contract.DecodeDemotionProfileReadReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if profile != "" {
					t.Fatalf("profile = %q where none is committed", profile)
				}
			},
		},
		{
			name:  "learning_proposal_find_pending",
			stage: db2contract.StageLearningProposalFindPending,
			encode: func() ([]byte, error) {
				return db2contract.EncodeLearningProposalFindPendingRequest(
					"rules", "live-probe-key", 1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeLearningProposalFindPendingReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "file_modified_since",
			stage: db2contract.StageFileModifiedSince,
			// The guarded cast has to plan: a regular expression, a timestamp
			// cast and a time-zone conversion, none of which a fake executes.
			encode: func() ([]byte, error) {
				return db2contract.EncodeFileModifiedSinceRequest(
					2147483000, "live-probe-no-such-file.c", 1)
			},
			decoded: func(t *testing.T, body []byte) {
				modified, err := db2contract.DecodeFileModifiedSinceReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if modified != 1 {
					t.Fatal("a file with no row read as unmodified; it would never " +
						"be indexed")
				}
			},
		},
		{
			name:  "projection_generations_list",
			stage: db2contract.StageProjectionGenerationsList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectionGenerationsListRequest("replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProjectionGenerationsListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "kb_ingest_queue_stats",
			stage:  db2contract.StageKBIngestQueueStats,
			encode: db2contract.EncodeKBIngestQueueStatsRequest,
			decoded: func(t *testing.T, body []byte) {
				// Four filtered aggregates in one pass; a fake cannot run
				// FILTER at all.
				if _, _, _, _, err := db2contract.DecodeKBIngestQueueStatsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_entities_list",
			stage: db2contract.StageMemoryEntitiesList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryEntitiesListRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryEntitiesListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_id_key_content",
			stage: db2contract.StageMemoryIDKeyContent,
			// Zero, the branch NULLIF covers: a plain LIMIT would answer empty
			// here and look identical to a schema with no memories.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryIDKeyContentRequest(0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryIDKeyContentReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_evidence_fields",
			stage: db2contract.StageMemoryEvidenceFields,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryEvidenceFieldsRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, err := db2contract.DecodeMemoryEvidenceFieldsReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a memory nothing holds reported as found")
				}
			},
		},
		{
			name:   "directive_counts_by_state",
			stage:  db2contract.StageDirectiveCountsByState,
			encode: db2contract.EncodeDirectiveCountsByStateRequest,
			decoded: func(t *testing.T, body []byte) {
				// Four filtered aggregates in one pass, which a fake cannot run.
				if _, _, _, _, err := db2contract.DecodeDirectiveCountsByStateReply(
					body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "feature_row_read",
			stage: db2contract.StageFeatureRowRead,
			encode: func() ([]byte, error) {
				return db2contract.EncodeFeatureRowReadRequest(
					"live-probe-subject", "memory", "v1")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeFeatureRowReadReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_conflicting_l2",
			stage: db2contract.StageMemoryConflictingL2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryConflictingL2Request(
					"live-probe-no-such-key", "live probe content")
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, err := db2contract.DecodeMemoryConflictingL2Reply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a key nothing holds reported a conflict")
				}
			},
		},
		{
			name:  "memory_temporal_refs_list",
			stage: db2contract.StageMemoryTemporalRefsList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryTemporalRefsListRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryTemporalRefsListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_superseded_keys",
			stage: db2contract.StageMemorySupersededKeys,
			// SUBSTR over STRPOS, grouped by the same expression, with the
			// count repeated in HAVING because PostgreSQL refuses the alias
			// there. None of that is something a fake evaluates.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySupersededKeysRequest(2, 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySupersededKeysReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "session_memories",
			stage: db2contract.StageSessionMemories,
			encode: func() ([]byte, error) {
				return db2contract.EncodeSessionMemoriesRequest("live-probe-session", 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeSessionMemoriesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "prospective_counts",
			stage:  db2contract.StageProspectiveCounts,
			encode: db2contract.EncodeProspectiveCountsRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, err := db2contract.DecodeProspectiveCountsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "document_hash_exists",
			stage: db2contract.StageDocumentHashExists,
			// A UNION over two tables, both joined to projects for the
			// generation. The generation pinning is the part a fake cannot
			// check.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDocumentHashExistsRequest(
					liveProbeScopeProject, "live-probe-hash")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, err := db2contract.DecodeDocumentHashExistsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_summaries_list",
			stage: db2contract.StageMemorySummariesList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySummariesListRequest(900012, 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySummariesListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_l1_session_clusters",
			stage: db2contract.StageMemoryL1SessionClusters,
			// GROUP BY with a HAVING over the same COUNT(*), which is the shape
			// PostgreSQL is particular about.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryL1SessionClustersRequest(
					"live-probe-session", 3)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryL1SessionClustersReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "memory_artifact_hashed_list",
			stage:  db2contract.StageMemoryArtifactHashedList,
			encode: db2contract.EncodeMemoryArtifactHashedListRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryArtifactHashedListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_purge_fence_read",
			stage: db2contract.StageKBPurgeFenceRead,
			// A self-join over kb_runtime_state with a cast from text to
			// timestamp and a parameterised interval. The cast is the part worth
			// running: a fake never evaluates it, and a heartbeat written in the
			// wrong format would only fail here.
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBPurgeFenceReadRequest(liveProbeFenceProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, err := db2contract.DecodeKBPurgeFenceReadReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_file_index_get",
			stage: db2contract.StageKBFileIndexGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBFileIndexGetRequest(
					liveProbeScopeProject, "docs/live-probe.md")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, err := db2contract.DecodeKBFileIndexGetReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_state_fields",
			stage: db2contract.StageMemoryStateFields,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryStateFieldsRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, err := db2contract.DecodeMemoryStateFieldsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_provenance_by_id",
			stage: db2contract.StageMemoryProvenanceByID,
			// The absent answer, which has to come back as absent rather than as
			// failed -- and pgx.ErrNoRows is what separates them, so only a real
			// database says which one this is.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryProvenanceByIDRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				result, _, _, _, err := db2contract.DecodeMemoryProvenanceByIDReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if result != provenanceAbsent {
					t.Fatalf("a memory that does not exist answered %d, want absent", result)
				}
			},
		},
		{
			name:  "memory_unit_active_meta",
			stage: db2contract.StageMemoryUnitActiveMeta,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryUnitActiveMetaRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, err := db2contract.DecodeMemoryUnitActiveMetaReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "lifecycle_counts",
			stage:  db2contract.StageLifecycleCounts,
			encode: db2contract.EncodeLifecycleCountsRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, err := db2contract.DecodeLifecycleCountsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "tool_registry_lookup",
			stage: db2contract.StageToolRegistryLookup,
			encode: func() ([]byte, error) {
				return db2contract.EncodeToolRegistryLookupRequest("live-probe-tool")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, err := db2contract.DecodeToolRegistryLookupReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "calibration_surface_list",
			stage: db2contract.StageCalibrationSurfaceList,
			// A join, a four-column grouping, a HAVING over a repeated COUNT and
			// an ORDER BY naming the alias that HAVING may not. Which of those
			// PostgreSQL accepts where is the whole question, and no fake
			// answers it.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCalibrationSurfaceListRequest(2)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCalibrationSurfaceListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "callers_find",
			stage: db2contract.StageCallersFind,
			// The unfiltered branch, which binds two parameters where the
			// filtered one binds three -- a statement built by concatenation is
			// exactly where that goes wrong.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCallersFindRequest("", "live_probe_callee")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCallersFindReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "callers_find_scoped",
			stage: db2contract.StageCallersFindScoped,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCallersFindScopedRequest(
					liveProbeScopeProject, "live_probe_callee")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCallersFindScopedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "callers_find_excluding_project",
			stage: db2contract.StageCallersFindExcludingProject,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCallersFindExcludingProjectRequest(
					liveProbeScopeProject, "live_probe_callee")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCallersFindExcludingProjectReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "file_definitions",
			stage: db2contract.StageFileDefinitions,
			encode: func() ([]byte, error) {
				return db2contract.EncodeFileDefinitionsRequest(
					liveProbeScopeProject, "src/live-probe.c")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeFileDefinitionsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "term_find",
			stage: db2contract.StageTermFind,
			// A grouping over five columns with a CASE leading the ordering.
			encode: func() ([]byte, error) {
				return db2contract.EncodeTermFindRequest("live_probe_symbol")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTermFindReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "l2_cross_key_pairs",
			stage: db2contract.StageL2CrossKeyPairs,
			// A self-join with a LIKE pattern built by concatenating a SUBSTR of
			// a STRPOS. Every one of those is a function whose PostgreSQL
			// spelling differs from the sqlite the C grew up on, and none of
			// them is checked anywhere but here.
			encode: func() ([]byte, error) {
				return db2contract.EncodeL2CrossKeyPairsRequest(8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeL2CrossKeyPairsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "l2_fact_decision_pairs",
			stage: db2contract.StageL2FactDecisionPairs,
			encode: func() ([]byte, error) {
				return db2contract.EncodeL2FactDecisionPairsRequest(8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeL2FactDecisionPairsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_summarise_clusters",
			stage: db2contract.StageMemorySummariseClusters,
			// AVG over a grouped read with the count repeated in HAVING.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySummariseClustersRequest(0.4, 3)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemorySummariseClustersReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_prior_in_session",
			stage: db2contract.StageMemoryPriorInSession,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryPriorInSessionRequest(
					"live-probe-session", 2147483000, 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryPriorInSessionReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "entity_top_triples",
			stage:  db2contract.StageEntityTopTriples,
			encode: db2contract.EncodeEntityTopTriplesRequest,
			// SELECT DISTINCT ordered by a selected column. Two statements in
			// this port ordered by an unselected one and PostgreSQL refused
			// both outright, so this is the probe that says this one is not a
			// third.
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityTopTriplesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "memory_lint",
			stage:  db2contract.StageMemoryLint,
			encode: db2contract.EncodeMemoryLintRequest,
			// Three statements in one operation, two of them with subqueries
			// over memory_links and one with a correlated NOT EXISTS. Nothing
			// but a real planner says whether all three run.
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryLintReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "witness_checkpoint_anchor_coverage",
			stage: db2contract.StageWitnessCheckpointAnchorCoverage,
			// encode() over the first element of an array_agg of a bytea, and a
			// bytea parameter on the other side of the comparison. Both are
			// places where pgx and libpq differ about types.
			encode: func() ([]byte, error) {
				return db2contract.EncodeWitnessCheckpointAnchorCoverageRequest(
					"00112233445566778899aabbccddeeff")
			},
			decoded: func(t *testing.T, body []byte) {
				read, _, _, err :=
					db2contract.DecodeWitnessCheckpointAnchorCoverageReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if read != 1 {
					t.Fatal("coverage could not be read against a real schema")
				}
			},
		},
		{
			name:  "enrollment_authority_resolve",
			stage: db2contract.StageEnrollmentAuthorityResolve,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEnrollmentAuthorityResolveRequest(
					"live-probe-fingerprint", "CN=live-probe", "01ab")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, err := db2contract.DecodeEnrollmentAuthorityResolveReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "memory_conflict_list",
			stage:  db2contract.StageMemoryConflictList,
			encode: db2contract.EncodeMemoryConflictListRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryConflictListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_event_frames_list",
			stage: db2contract.StageMemoryEventFramesList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryEventFramesListRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryEventFramesListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_provenance_list",
			stage: db2contract.StageMemoryProvenanceList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryProvenanceListRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryProvenanceListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_lookup_by_key",
			stage: db2contract.StageMemoryLookupByKey,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryLookupByKeyRequest("live-probe-missing-key")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, err := db2contract.DecodeMemoryLookupByKeyReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "demotion_score",
			stage: db2contract.StageDemotionScore,
			// A data-modifying CTE whose UPDATE runs whether or not the outer
			// query reads it, and an interval arithmetic expression over a text
			// column cast to a timestamp. Neither is something a fake evaluates.
			//
			// It is a read entry despite stamping rows: the stamp is on
			// artifacts the probe does not create, and against an empty window
			// there is nothing to stamp.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDemotionScoreRequest(2147483000, 64, 30.0, 5)
			},
			decoded: func(t *testing.T, body []byte) {
				valid, _, err := db2contract.DecodeDemotionScoreReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if valid != 0 {
					t.Fatal("a row with no attributions scored as though it had some")
				}
			},
		},
		{
			name:   "kb_async_queue_status",
			stage:  db2contract.StageKBAsyncQueueStatus,
			encode: db2contract.EncodeKBAsyncQueueStatusRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, err :=
					db2contract.DecodeKBAsyncQueueStatusReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "fidelity_report_by_turn",
			stage: db2contract.StageFidelityReportByTurn,
			encode: func() ([]byte, error) {
				return db2contract.EncodeFidelityReportByTurnRequest("live-probe-turn")
			},
			decoded: func(t *testing.T, body []byte) {
				result, _, _, _, _, err :=
					db2contract.DecodeFidelityReportByTurnReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if result != 0 {
					t.Fatal("a turn with no report answered as though it had one")
				}
			},
		},
		{
			name:  "term_find_in_project",
			stage: db2contract.StageTermFindInProject,
			// Three statements against an empty index: the exact search finds
			// nothing, so both LIKE tiers run too. That is the only way to see
			// all three plan.
			encode: func() ([]byte, error) {
				return db2contract.EncodeTermFindInProjectRequest(
					liveProbeScopeProject, "live_probe_symbol")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTermFindInProjectReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "term_find_excluding_project",
			stage: db2contract.StageTermFindExcludingProject,
			encode: func() ([]byte, error) {
				return db2contract.EncodeTermFindExcludingProjectRequest(
					liveProbeScopeProject, "live_probe_symbol")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTermFindExcludingProjectReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_edges_for_entity",
			stage: db2contract.StageEntityEdgesForEntity,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityEdgesForEntityRequest("live-probe-entity", 32)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityEdgesForEntityReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_edges_by_token",
			stage: db2contract.StageEntityEdgesByToken,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityEdgesByTokenRequest("live-probe-token", 32)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityEdgesByTokenReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_neighbors_weighted",
			stage: db2contract.StageEntityNeighborsWeighted,
			// A regex guard over a text column feeding an interval subtraction,
			// inside a union wrapped in a subselect. Whether PostgreSQL accepts
			// that shape is the whole question here.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityNeighborsWeightedRequest(
					"live-probe-entity", 32, 1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityNeighborsWeightedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_neighbors_filtered",
			stage: db2contract.StageEntityNeighborsFiltered,
			// The ordered branch, because the sort is appended to a statement
			// built by concatenation and the LIMIT parameter follows it.
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityNeighborsFilteredRequest(
					"live-probe-entity", "depends_on", "used_by", 1, 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityNeighborsFilteredReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_units_list",
			stage: db2contract.StageMemoryUnitsList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryUnitsListRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryUnitsListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "task_edges",
			stage: db2contract.StageTaskEdges,
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskEdgesRequest(2147483000, 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTaskEdgesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "anti_pattern_list",
			stage:  db2contract.StageAntiPatternList,
			encode: db2contract.EncodeAntiPatternListRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeAntiPatternListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "console_oidc_get",
			stage:  db2contract.StageConsoleOidcGet,
			encode: db2contract.EncodeConsoleOidcGetRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, _, err :=
					db2contract.DecodeConsoleOidcGetReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "projection_generation_meta",
			stage: db2contract.StageProjectionGenerationMeta,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectionGenerationMetaRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, err :=
					db2contract.DecodeProjectionGenerationMetaReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a generation that does not exist answered as found")
				}
			},
		},
		{
			name:  "projection_edges",
			stage: db2contract.StageProjectionEdges,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectionEdgesRequest(liveProbeScopeProject, 64)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProjectionEdgesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "projection_edges_for_generation",
			stage: db2contract.StageProjectionEdgesForGeneration,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProjectionEdgesForGenerationRequest(2147483000, 64)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err :=
					db2contract.DecodeProjectionEdgesForGenerationReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "bandit_arm_stats_read",
			stage: db2contract.StageBanditArmStatsRead,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditArmStatsReadRequest(
					"live-probe-decision", "live-probe-arm")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, err :=
					db2contract.DecodeBanditArmStatsReadReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "corpus_pipeline_status",
			stage:  db2contract.StageCorpusPipelineStatus,
			encode: db2contract.EncodeCorpusPipelineStatusRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, _, err :=
					db2contract.DecodeCorpusPipelineStatusReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_project_status",
			stage: db2contract.StageKBProjectStatus,
			// Four counts from two tables in one statement, one of them a
			// scalar subquery over a three-way join.
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBProjectStatusRequest(liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, err := db2contract.DecodeKBProjectStatusReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 1 {
					t.Fatal("the counts could not be read against a real schema")
				}
			},
		},
		{
			name:   "kb_reembed_status",
			stage:  db2contract.StageKBReembedStatus,
			encode: db2contract.EncodeKBReembedStatusRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, _, err :=
					db2contract.DecodeKBReembedStatusReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_release_read",
			stage: db2contract.StageKBReleaseRead,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBReleaseReadRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, err := db2contract.DecodeKBReleaseReadReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a release that does not exist answered as found")
				}
			},
		},
		{
			name:  "mining_job_get",
			stage: db2contract.StageMiningJobGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMiningJobGetRequest("live-probe-job")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, err := db2contract.DecodeMiningJobGetReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_ingest_queue_recent",
			stage: db2contract.StageKBIngestQueueRecent,
			// A CASE in the ordering followed by a COALESCE with an explicit
			// null placement, which is the shape PostgreSQL is particular
			// about.
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBIngestQueueRecentRequest(20)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeKBIngestQueueRecentReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "lifecycle_unresolved_contradictions",
			stage: db2contract.StageLifecycleUnresolvedContradictions,
			// Two scope filters and two scope ranks over one set of four
			// placeholders, inside an ordering that compares the ranks. Whether
			// PostgreSQL accepts a placeholder reused that many times in that
			// many positions is exactly what a fake cannot say.
			encode: func() ([]byte, error) {
				return db2contract.EncodeLifecycleUnresolvedContradictionsRequest(
					liveProbeScopeFlags, liveProbeWorkspace, liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err :=
					db2contract.DecodeLifecycleUnresolvedContradictionsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_dedupe_candidates",
			stage: db2contract.StageMemoryDedupeCandidates,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryDedupeCandidatesRequest("fact")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryDedupeCandidatesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "anti_pattern_list_hot",
			stage: db2contract.StageAntiPatternListHot,
			encode: func() ([]byte, error) {
				return db2contract.EncodeAntiPatternListHotRequest(5)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeAntiPatternListHotReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "calibration_conformal_window",
			stage: db2contract.StageCalibrationConformalWindow,
			// The scope predicates are two OR chains over the same parameter,
			// which is where one statement replaces the C's three.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCalibrationConformalWindowRequest(
					"recall", "synthesis", "project", liveProbeScopeProject, 128)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err :=
					db2contract.DecodeCalibrationConformalWindowReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "code_search",
			stage: db2contract.StageCodeSearch,
			// ts_headline and ts_rank over a generated tsvector column, with the
			// query parsed by plainto_tsquery. None of that exists outside a
			// real PostgreSQL.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCodeSearchRequest(
					"live probe symbol", liveProbeScopeProject, 1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCodeSearchReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "code_search_excluding_project",
			stage: db2contract.StageCodeSearchExcludingProject,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCodeSearchExcludingProjectRequest(
					"live probe symbol", liveProbeScopeProject, 0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err :=
					db2contract.DecodeCodeSearchExcludingProjectReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_walk_step_typed",
			stage: db2contract.StageEntityWalkStepTyped,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityWalkStepTypedRequest("live-probe-entity")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityWalkStepTypedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "memory_low_effectiveness",
			stage: db2contract.StageMemoryLowEffectiveness,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryLowEffectivenessRequest(0.3, 32)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeMemoryLowEffectivenessReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "anti_pattern_check",
			stage: db2contract.StageAntiPatternCheck,
			encode: func() ([]byte, error) {
				return db2contract.EncodeAntiPatternCheckRequest(
					"src/live-probe.c", "rm -rf /tmp/live-probe")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeAntiPatternCheckReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "artifact_list_proposed",
			stage: db2contract.StageArtifactListProposed,
			// A correlated string_agg with its own ORDER BY inside the
			// aggregate, which is the shape that replaces the C's query per row.
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactListProposedRequest("recall", 20)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeArtifactListProposedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "rules_list",
			stage:  db2contract.StageRulesList,
			encode: db2contract.EncodeRulesListRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRulesListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "rules_list_hard",
			stage:  db2contract.StageRulesListHard,
			encode: db2contract.EncodeRulesListHardRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRulesListHardReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "rules_list_by_tier",
			stage: db2contract.StageRulesListByTier,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesListByTierRequest(75)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRulesListByTierReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "rules_find_by_title",
			stage: db2contract.StageRulesFindByTitle,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesFindByTitleRequest("live probe rule")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeRulesFindByTitleReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "task_get",
			stage: db2contract.StageTaskGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, err := db2contract.DecodeTaskGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a task that does not exist answered as found")
				}
			},
		},
		{
			name:  "task_subtasks",
			stage: db2contract.StageTaskSubtasks,
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskSubtasksRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTaskSubtasksReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_node_get",
			stage: db2contract.StageEntityNodeGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityNodeGetRequest("node:live-probe-missing")
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeEntityNodeGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a node that does not exist answered as found")
				}
			},
		},
		{
			name:  "entity_edge_explain",
			stage: db2contract.StageEntityEdgeExplain,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityEdgeExplainRequest("live-probe-entity")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityEdgeExplainReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_doc_regions_for_chunk",
			stage: db2contract.StageKBDocRegionsForChunk,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocRegionsForChunkRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeKBDocRegionsForChunkReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_document_fetch",
			stage: db2contract.StageKBDocumentFetch,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocumentFetchRequest(2147483000, "")
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeKBDocumentFetchReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a chunk that does not exist answered as found")
				}
			},
		},
		{
			name:  "prospective_list",
			stage: db2contract.StageProspectiveList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveListRequest("armed", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProspectiveListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "prospective_list_armed",
			stage:  db2contract.StageProspectiveListArmed,
			encode: db2contract.EncodeProspectiveListArmedRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProspectiveListArmedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "prospective_by_entity",
			stage: db2contract.StageProspectiveByEntity,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveByEntityRequest("live-probe-entity", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProspectiveByEntityReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "prospective_by_file",
			stage: db2contract.StageProspectiveByFile,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveByFileRequest("docs/live-probe.md", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeProspectiveByFileReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "prospective_by_trigger_terms",
			stage: db2contract.StageProspectiveByTriggerTerms,
			// A tsquery built in Go and parsed by PostgreSQL. A term the
			// builder emitted wrongly fails here and nowhere else, which is the
			// whole reason this probe exists.
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveByTriggerTermsRequest(
					"we should rotate the live probe key", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err :=
					db2contract.DecodeProspectiveByTriggerTermsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "directive_get",
			stage: db2contract.StageDirectiveGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeDirectiveGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a directive that does not exist answered as found")
				}
			},
		},
		{
			name:  "directive_list",
			stage: db2contract.StageDirectiveList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveListRequest("open", "", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDirectiveListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "directive_by_entity",
			stage: db2contract.StageDirectiveByEntity,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveByEntityRequest("live-probe-entity", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDirectiveByEntityReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "directive_by_file",
			stage: db2contract.StageDirectiveByFile,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveByFileRequest("docs/live-probe.md", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDirectiveByFileReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "directive_by_lexical",
			stage: db2contract.StageDirectiveByLexical,
			// The statement is built at request time with one placeholder per
			// surviving term, so the placeholder numbering is only checkable
			// against a real parser.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDirectiveByLexicalRequest(
					`"vault" OR "rotation" OR "probe"`, 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDirectiveByLexicalReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "decision_log_get",
			stage: db2contract.StageDecisionLogGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeDecisionLogGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a decision that does not exist answered as found")
				}
			},
		},
		{
			name:  "decision_log_list",
			stage: db2contract.StageDecisionLogList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogListRequest("", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDecisionLogListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "decision_log_list_scoped",
			stage: db2contract.StageDecisionLogListScoped,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogListScopedRequest(
					"live-probe-subject", "active", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDecisionLogListScopedReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "learning_proposal_get",
			stage: db2contract.StageLearningProposalGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeLearningProposalGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeLearningProposalGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a proposal that does not exist answered as found")
				}
			},
		},
		{
			name:  "task_list",
			stage: db2contract.StageTaskList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskListRequest("todo", "", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTaskListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_doc_read",
			stage: db2contract.StageKBDocRead,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocReadRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeKBDocReadReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a document that does not exist answered as found")
				}
			},
		},
		{
			name:  "kb_doc_list_review",
			stage: db2contract.StageKBDocListReview,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocListReviewRequest(16, 0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeKBDocListReviewReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_doc_assets_list",
			stage: db2contract.StageKBDocAssetsList,
			// A DISTINCT over ten columns joined to a correlated subquery for
			// the generation. Nothing but a real planner accepts or refuses it.
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocAssetsListRequest(
					liveProbeScopeProject, "docs/live-probe.pdf")
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeKBDocAssetsListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "kb_async_job_get",
			stage: db2contract.StageKBAsyncJobGet,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBAsyncJobGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeKBAsyncJobGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatal("a job that does not exist answered as found")
				}
			},
		},
		{
			name:  "typed_fact_recall",
			stage: db2contract.StageTypedFactRecall,
			// The ordering is a CASE over the filter parameter, which is where
			// one statement replaces the C's two.
			encode: func() ([]byte, error) {
				return db2contract.EncodeTypedFactRecallRequest("live-probe-subject", "", 16)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeTypedFactRecallReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "calibration_audit_stats",
			stage: db2contract.StageCalibrationAuditStats,
			// A CAST of a float to an int inside a CASE, grouped and ordered by
			// its own position. Whether PostgreSQL accepts that shape is the
			// question, and the bucket arithmetic is the answer's meaning.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCalibrationAuditStatsRequest(
					"recall", "synthesis", "project", liveProbeScopeProject, 128)
			},
			decoded: func(t *testing.T, body []byte) {
				buckets, err := db2contract.DecodeCalibrationAuditStatsReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if len(buckets) != db2contract.CalibrationAuditStatsMaxRows {
					t.Fatalf("buckets = %d, want every one", len(buckets))
				}
			},
		},
		{
			name:  "css_token_candidates",
			stage: db2contract.StageCssTokenCandidates,
			// A four-way join over the stylesheet tables, which only exist in a
			// real schema.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCssTokenCandidatesRequest(liveProbeScopeProject, 2)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCssTokenCandidatesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "css_migration_rules_doc",
			stage: db2contract.StageCssMigrationRulesDoc,
			// Three counts in one statement, two of them FILTERed and one of
			// those carrying a LIKE with an ESCAPE clause. Whether that shape
			// runs is the question.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCssMigrationRulesDocRequest(liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				document, err := db2contract.DecodeCssMigrationRulesDocReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if !strings.Contains(document, "Indexed rules in exemplar") {
					t.Fatalf("document = %q", document)
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
		{
			name:  "artifact_cite",
			stage: db2contract.StageArtifactCite,
			// artifact_citations carries a foreign key into artifacts, so the
			// citing artifact has to exist for the insert to reach its own
			// conflict clause.
			seed:   []string{liveProbeArtifact},
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactCiteRequest(
					liveProbeArtifactID, "kb_document", "900010")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeArtifactCiteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the citation was not recorded")
				}
			},
		},
		{
			name:  "artifact_link",
			stage: db2contract.StageArtifactLink,
			// Both ends have to exist, so a second artifact is seeded beside
			// the first.
			seed:   []string{liveProbeArtifact, liveProbeArtifactTarget},
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactLinkRequest(
					liveProbeArtifactID, liveProbeArtifactTargetID, "supersedes")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeArtifactLinkReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the link was not recorded")
				}
			},
		},
		{
			name:  "collab_rule_propose",
			stage: db2contract.StageCollabRulePropose,
			// The gating WHERE has to plan and admit: a rule is expected back,
			// which is what tells an INSERT ... SELECT that inserted from one
			// that did not.
			encode: func() ([]byte, error) {
				return db2contract.EncodeCollabRuleProposeRequest(
					"live probe rule", "live probe reason", "live-probe")
			},
			decoded: func(t *testing.T, body []byte) {
				id, err := db2contract.DecodeCollabRuleProposeReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if id == 0 {
					t.Fatal("no rule was raised; the cap subquery may have refused it")
				}
			},
		},
		{
			name:  "task_update_state",
			stage: db2contract.StageTaskUpdateState,
			seed:  []string{liveProbeTask},
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskUpdateStateRequest(liveProbeTaskID, "done")
			},
			decoded: func(t *testing.T, body []byte) {
				changed, err := db2contract.DecodeTaskUpdateStateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if changed != 1 {
					t.Fatal("a task that exists did not change state")
				}
			},
		},
		{
			name:  "code_file_upsert",
			stage: db2contract.StageCodeFileUpsert,
			// Seeded, because the INSERT ... SELECT takes its generation from a
			// current project row: without one it inserts nothing and the probe
			// would exercise the refusal rather than the upsert.
			//
			// Twice, so the ON CONFLICT path runs and refreshes the derived
			// columns.
			repeat: 2,
			seed:   []string{liveProbeUpsertProject},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCodeFileUpsertRequest(
					liveProbeUpsertProjectID, "vendor/pkg/main.go", "2026-01-01 00:00:00")
			},
			decoded: func(t *testing.T, body []byte) {
				id, err := db2contract.DecodeCodeFileUpsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if id == 0 {
					t.Fatal("no file row came back for a current project")
				}
			},
		},
		{
			name:  "async_enqueue",
			stage: db2contract.StageAsyncEnqueue,
			// Twice: the second takes the conflict path, which is what stops a
			// duplicate job being queued.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeAsyncEnqueueRequest(
					"extract_doc", 900011, "replay-project")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeAsyncEnqueueReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the enqueue did not run")
				}
			},
		},
		{
			name:  "kb_documents_set_tsr_state",
			stage: db2contract.StageKBDocumentsSetTsrState,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocumentsSetTsrStateRequest(
					"replay-project", "live-probe-no-such-file.pdf", "done")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBDocumentsSetTsrStateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "lifecycle_update_state",
			stage: db2contract.StageLifecycleUpdateState,
			// Four CASE expressions have to plan, including the one comparing
			// against an empty valid_until.
			encode: func() ([]byte, error) {
				return db2contract.EncodeLifecycleUpdateStateRequest(
					2147483000, "archived", "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeLifecycleUpdateStateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "memory_alias_insert",
			stage: db2contract.StageMemoryAliasInsert,
			// Seeded because memory_aliases carries a foreign key into
			// memories, and run twice so the conflict path executes.
			repeat: 2,
			seed:   []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryAliasInsertRequest(
					liveProbeAliasMemoryID, "live-probe-alias", 0.8)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryAliasInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the alias was not recorded")
				}
			},
		},
		{
			name:   "memory_episode_card_insert",
			stage:  db2contract.StageMemoryEpisodeCardInsert,
			repeat: 2,
			seed:   []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryEpisodeCardInsertRequest(
					liveProbeAliasMemoryID, "live-probe-key", "live probe card")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryEpisodeCardInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the card was not recorded")
				}
			},
		},
		{
			name:  "evidence_store_vector",
			stage: db2contract.StageEvidenceStoreVector,
			// evidence_vectors carries a foreign key into artifacts, and
			// embedding is halfvec(384) -- so the literal has to parse AND
			// carry the right number of dimensions, neither of which a fake
			// checks. The first draft of this probe sent "[]" and failed here,
			// which is how the dimension requirement got written down.
			//
			// Run twice so the delete half has something to remove.
			repeat: 2,
			seed:   []string{liveProbeArtifact},
			encode: func() ([]byte, error) {
				return db2contract.EncodeEvidenceStoreVectorRequest(
					liveProbeArtifactID, "evidence", liveProbeEmbedding())
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeEvidenceStoreVectorReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the vector was not stored")
				}
			},
		},
		{
			name:  "mining_job_complete",
			stage: db2contract.StageMiningJobComplete,
			encode: func() ([]byte, error) {
				return db2contract.EncodeMiningJobCompleteRequest(
					"live-probe-job", 400, "")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMiningJobCompleteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "kb_directive_resolve",
			stage: db2contract.StageKBDirectiveResolve,
			// Seeded open, because the predicate is now in the WHERE: unseeded
			// this would probe the refusal rather than the resolve.
			seed: []string{liveProbeDirective},
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDirectiveResolveRequest(
					liveProbeDirectiveID, 1, "live probe note")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBDirectiveResolveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("an open directive was not resolved")
				}
			},
		},
		{
			name:  "resolve_contradiction",
			stage: db2contract.StageResolveContradiction,
			// A contradiction directive naming the pair, so the symmetric
			// match has something to find.
			seed: []string{liveProbeContradiction},
			encode: func() ([]byte, error) {
				// Deliberately the reverse ordering of the seeded pair, which
				// is what exercises the symmetric half of the predicate.
				return db2contract.EncodeResolveContradictionRequest(
					liveProbeContradictionB, liveProbeContradictionA, 1)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeResolveContradictionReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the pair did not resolve in the reverse ordering")
				}
			},
		},
		{
			name:  "memory_link_create",
			stage: db2contract.StageMemoryLinkCreate,
			// memory_links carries foreign keys into memories on both ends, so
			// two memories are seeded. Twice, for the conflict path.
			repeat: 2,
			seed:   []string{liveProbeAliasMemory, liveProbeLinkTarget},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryLinkCreateRequest(
					liveProbeAliasMemoryID, liveProbeLinkTargetID, "depends_on")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryLinkCreateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the link was not recorded")
				}
			},
		},
		{
			name:   "memory_scope_tag_insert",
			stage:  db2contract.StageMemoryScopeTagInsert,
			repeat: 2,
			seed:   []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryScopeTagInsertRequest(
					liveProbeAliasMemoryID, "project", "live-probe-project")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryScopeTagInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the scope tag was not recorded")
				}
			},
		},
		{
			name:  "memory_mark_merged_into",
			stage: db2contract.StageMemoryMarkMergedInto,
			// Five predicates have to plan together; nothing needs to match for
			// that.
			seed: []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryMarkMergedIntoRequest(
					liveProbeAliasMemoryID, "live-probe-session", 0.3)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryMarkMergedIntoReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the update did not run")
				}
			},
		},
		{
			name:  "purge_fence_heartbeat",
			stage: db2contract.StagePurgeFenceHeartbeat,
			// Twice, because the second heartbeat is the one that takes the
			// ON CONFLICT branch -- the first inserts the timestamp row.
			repeat: 2,
			seed:   []string{liveProbeFence},
			encode: func() ([]byte, error) {
				return db2contract.EncodePurgeFenceHeartbeatRequest(
					liveProbeFenceProject, liveProbeFenceGeneration, liveProbeFencePurgeID)
			},
			decoded: func(t *testing.T, body []byte) {
				applied, err := db2contract.DecodePurgeFenceHeartbeatReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if applied != 1 {
					t.Fatal("the fence this purge holds was not refreshed")
				}
			},
		},
		{
			name:  "purge_fence_clear",
			stage: db2contract.StagePurgeFenceClear,
			// Not repeated: the second clear correctly finds no fence, and a
			// probe that asserted it applied would be asserting the wrong thing.
			seed: []string{liveProbeFence},
			encode: func() ([]byte, error) {
				return db2contract.EncodePurgeFenceClearRequest(
					liveProbeFenceProject, liveProbeFenceGeneration, liveProbeFencePurgeID)
			},
			decoded: func(t *testing.T, body []byte) {
				applied, err := db2contract.DecodePurgeFenceClearReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if applied != 1 {
					t.Fatal("the fence this purge holds was not released")
				}
			},
		},
		{
			name:  "kb_ingest_queue_complete",
			stage: db2contract.StageKBIngestQueueComplete,
			seed:  []string{liveProbeIngestJob},
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBIngestQueueCompleteRequest(900017, 7, 90, 88)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBIngestQueueCompleteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the completion did not run")
				}
			},
		},
		{
			name:  "kb_doc_set_state",
			stage: db2contract.StageKBDocSetState,
			// The clearing branch, because it is the one that writes three
			// columns and the one review_needed's type has to accept a boolean
			// literal for.
			seed: []string{liveProbeReviewDoc},
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBDocSetStateRequest(
					900018, "published", 1, "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBDocSetStateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the state change did not run")
				}
			},
		},
		{
			name:  "memory_set_artifact",
			stage: db2contract.StageMemorySetArtifact,
			// A memory to point at, because the reply is the changed-row count:
			// against an empty schema this would answer zero and prove nothing.
			seed: []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemorySetArtifactRequest(
					liveProbeAliasMemoryID, "file", "docs/live-probe.md", "")
			},
			decoded: func(t *testing.T, body []byte) {
				changed, err := db2contract.DecodeMemorySetArtifactReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if changed != 1 {
					t.Fatal("the artifact was not attached")
				}
			},
		},
		{
			name:  "memory_entity_insert",
			stage: db2contract.StageMemoryEntityInsert,
			// Twice, for the ON CONFLICT branch -- which needs a real unique
			// constraint to take, and there is no fake for that.
			repeat: 2,
			seed:   []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryEntityInsertRequest(
					liveProbeAliasMemoryID, "postgres", "mention", 0.5)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryEntityInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the entity insert did not run")
				}
			},
		},
		{
			name:   "memory_temporal_insert",
			stage:  db2contract.StageMemoryTemporalInsert,
			repeat: 2,
			seed:   []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryTemporalInsertRequest(
					liveProbeAliasMemoryID, "last tuesday", "date_phrase", 0.5)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryTemporalInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the temporal insert did not run")
				}
			},
		},
		{
			name:  "task_create",
			stage: db2contract.StageTaskCreate,
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskCreateRequest(
					"live probe task", "live-probe-session", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				taskID, err := db2contract.DecodeTaskCreateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if taskID == 0 {
					t.Fatal("no task was created")
				}
			},
		},
		{
			name:  "task_add_edge",
			stage: db2contract.StageTaskAddEdge,
			// Both ends are foreign keys into tasks, so two tasks are seeded --
			// without them the insert fails and the probe would be asserting
			// that a broken write is reported as broken.
			seed: []string{liveProbeEdgeSource, liveProbeEdgeTarget},
			encode: func() ([]byte, error) {
				return db2contract.EncodeTaskAddEdgeRequest(900019, 900020, "depends_on")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeTaskAddEdgeReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the edge was not recorded")
				}
			},
		},
		{
			name:  "recompute_blocked_symbols",
			stage: db2contract.StageRecomputeBlockedSymbols,
			// The meta row has to exist for the version bump to find something,
			// and the fill statement joins four tables with two grouped arms
			// unioned -- none of which a fake plans.
			seed: []string{liveProbeCrossRepoMeta},
			encode: func() ([]byte, error) {
				return db2contract.EncodeRecomputeBlockedSymbolsRequest(3, 2, 4)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeRecomputeBlockedSymbolsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "rules_insert",
			stage: db2contract.StageRulesInsert,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesInsertRequest(
					"positive", "live probe rule", "live probe", 60)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeRulesInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the rule was not recorded")
				}
			},
		},
		{
			name:  "rules_reinforce_directive",
			stage: db2contract.StageRulesReinforceDirective,
			seed:  []string{liveProbeRule},
			encode: func() ([]byte, error) {
				return db2contract.EncodeRulesReinforceDirectiveRequest(
					liveProbeRuleID, "hard", 1, 80)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeRulesReinforceDirectiveReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the reinforcement did not run")
				}
			},
		},
		{
			name:  "demotion_profile_write",
			stage: db2contract.StageDemotionProfileWrite,
			// The payload column is JSONB and the parameter is text, so this is
			// the probe that proves the cast is there: without it PostgreSQL
			// refuses the insert outright.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDemotionProfileWriteRequest(
					"episodic", "project", "live-probe-project", `{"half_life_days":30}`)
			},
			decoded: func(t *testing.T, body []byte) {
				profileID, err := db2contract.DecodeDemotionProfileWriteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if profileID == "" {
					t.Fatal("no profile was written")
				}
			},
		},
		{
			name:  "retrieval_attribution_write",
			stage: db2contract.StageRetrievalAttributionWrite,
			encode: func() ([]byte, error) {
				return db2contract.EncodeRetrievalAttributionWriteRequest(
					"live-probe-event", 4321, "accepted", 0.25)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeRetrievalAttributionWriteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the attribution was not recorded")
				}
			},
		},
		{
			name:  "entity_profile_upsert",
			stage: db2contract.StageEntityProfileUpsert,
			// Twice, because the second call is the one that takes the conflict
			// branch -- and the branch is where the column list matters.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityProfileUpsertRequest(
					"entity:live-probe", "Live Probe", 12, `{"kind":"tool"}`)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeEntityProfileUpsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the profile was not written")
				}
			},
		},
		{
			name:  "entity_node_alias_upsert",
			stage: db2contract.StageEntityNodeAliasUpsert,
			// node_key is a foreign key into entity_nodes, so the node has to
			// exist. Twice again, for the conflict branch.
			repeat: 2,
			seed:   []string{liveProbeEntityNode},
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityNodeAliasUpsertRequest(
					"live-probe-alias", "node:live-probe", "abbreviation",
					liveProbeScopeProject, 7)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeEntityNodeAliasUpsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the alias was not written")
				}
			},
		},
		{
			name:  "console_oidc_put",
			stage: db2contract.StageConsoleOidcPut,
			// Twice, for the conflict branch: the row is a singleton, so the
			// second call is the only one that exercises the replacement.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeConsoleOidcPutRequest(
					"https://live-probe", "aimee-console",
					"https://live-probe/jwks", "groups", "admins")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeConsoleOidcPutReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the settings were not stored")
				}
			},
		},
		{
			name:  "memory_lineage_insert",
			stage: db2contract.StageMemoryLineageInsert,
			// The identifier comes back from the insert, so this is the probe
			// that says RETURNING actually returns.
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryLineageInsertRequest(
					"memory", 900012, "ingest", "docs/live-probe.md", 0.8)
			},
			decoded: func(t *testing.T, body []byte) {
				lineageID, err := db2contract.DecodeMemoryLineageInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if lineageID == 0 {
					t.Fatal("no lineage record was written")
				}
			},
		},
		{
			name:  "memory_relation_insert",
			stage: db2contract.StageMemoryRelationInsert,
			// memory_id is a foreign key, so the memory has to exist.
			seed: []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryRelationInsertRequest(
					liveProbeAliasMemoryID, "aimee", "depends_on", "postgres",
					"aimee stores memories in postgres")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryRelationInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the relation was not recorded")
				}
			},
		},
		{
			name:  "anti_pattern_insert",
			stage: db2contract.StageAntiPatternInsert,
			encode: func() ([]byte, error) {
				return db2contract.EncodeAntiPatternInsertRequest(
					"live probe anti-pattern", "live probe", "review", "pr/1", 0.8)
			},
			decoded: func(t *testing.T, body []byte) {
				patternID, err := db2contract.DecodeAntiPatternInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if patternID == 0 {
					t.Fatal("no anti-pattern was written")
				}
			},
		},
		{
			name:  "workflow_pattern_insert",
			stage: db2contract.StageWorkflowPatternInsert,
			encode: func() ([]byte, error) {
				return db2contract.EncodeWorkflowPatternInsertRequest(
					"live probe pattern", "live probe", "review", "pr/1", 0.8)
			},
			decoded: func(t *testing.T, body []byte) {
				patternID, err := db2contract.DecodeWorkflowPatternInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if patternID == 0 {
					t.Fatal("no workflow pattern was written")
				}
			},
		},
		{
			name:  "feedback_record",
			stage: db2contract.StageFeedbackRecord,
			// Twice: the first call inserts and the second reinforces, and the
			// reinforcement is the branch with the subselect and the LEAST in
			// it. Both must report a rule, so the assertion covers both.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeFeedbackRecordRequest(
					"positive", "live probe feedback", "live probe", 60)
			},
			decoded: func(t *testing.T, body []byte) {
				ruleID, _, err := db2contract.DecodeFeedbackRecordReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if ruleID == 0 {
					t.Fatal("no rule was recorded")
				}
			},
		},
		{
			name:  "kb_ingest_queue_claim_next",
			stage: db2contract.StageKBIngestQueueClaimNext,
			// FOR UPDATE SKIP LOCKED inside a subselect feeding an UPDATE with
			// RETURNING. Nothing about that shape is checkable without a real
			// planner, and a pending job is seeded so the claim actually claims.
			seed:   []string{liveProbePendingIngestJob},
			encode: db2contract.EncodeKBIngestQueueClaimNextRequest,
			decoded: func(t *testing.T, body []byte) {
				claimed, jobID, _, _, _, _, err :=
					db2contract.DecodeKBIngestQueueClaimNextReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if claimed != 1 || jobID == 0 {
					t.Fatal("the pending job was not claimed")
				}
			},
		},
		{
			name:  "vector_index_op_record",
			stage: db2contract.StageVectorIndexOpRecord,
			// Twice, for the conflict branch: the attempt count and the
			// version-restamp CASE only run there.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeVectorIndexOpRecordRequest(
					900022, "kb_documents", 0, 1, "")
			},
			decoded: func(t *testing.T, body []byte) {
				recorded, err := db2contract.DecodeVectorIndexOpRecordReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if recorded != 1 {
					t.Fatal("the index operation was not recorded")
				}
			},
		},
		{
			name:  "artifact_reject",
			stage: db2contract.StageArtifactReject,
			// The audit event carries a foreign key into artifacts, so the
			// artifact has to exist -- and both snapshots are JSONB, which is
			// where the empty-string cast would fail.
			seed: []string{liveProbeRejectableArtifact},
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactRejectRequest(
					"live-probe-artifact", "wrong-scope", "project", "", "")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeArtifactRejectReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the rejection did not land")
				}
			},
		},
		{
			name:  "code_index_op_record",
			stage: db2contract.StageCodeIndexOpRecord,
			// Twice, for the conflict branch where the attempt count grows.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCodeIndexOpRecordRequest(
					900024, liveProbeScopeProject, "node:live-probe",
					"src/live-probe.c", 1, "")
			},
			decoded: func(t *testing.T, body []byte) {
				recorded, err := db2contract.DecodeCodeIndexOpRecordReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if recorded != 1 {
					t.Fatal("the index operation was not recorded")
				}
			},
		},
		{
			name:  "kb_audit_append",
			stage: db2contract.StageKBAuditAppend,
			// Twice, because the second append is the one that reads a tail
			// rather than starting from genesis -- and the table's WORM
			// triggers refuse anything but an insert, so a wrong sequence
			// number fails rather than overwriting.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeKBAuditAppendRequest(
					"admin", "live-probe", "reject", "live-probe-artifact",
					"denied", "live probe")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeKBAuditAppendReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the audit row was not appended")
				}
			},
		},
		{
			name:  "memory_coref_audit_insert",
			stage: db2contract.StageMemoryCorefAuditInsert,
			// memory_id is a foreign key, so the memory has to exist.
			seed: []string{liveProbeAliasMemory},
			encode: func() ([]byte, error) {
				return db2contract.EncodeMemoryCorefAuditInsertRequest(
					liveProbeAliasMemoryID, "live-probe-session", "resolved",
					"postgres", "exact", 0.8)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeMemoryCorefAuditInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the audit row was not written")
				}
			},
		},
		{
			name:  "cross_repo_set_trust",
			stage: db2contract.StageCrossRepoSetTrust,
			// The project and the meta row both have to exist: the first for
			// the row lock to hold something, the second for the epoch. Twice,
			// so the second call takes the no-op branch where the epoch is read
			// but not bumped.
			repeat: 2,
			seed:   []string{liveProbeTrustProject, liveProbeCrossRepoMeta},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCrossRepoSetTrustRequest(
					"live-probe-trust-project", "trusted", "live-probe", "req-1")
			},
			decoded: func(t *testing.T, body []byte) {
				result, _, _, err := db2contract.DecodeCrossRepoSetTrustReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if result != 0 {
					t.Fatalf("the trust write answered %d", result)
				}
			},
		},
		{
			name:  "enrollment_insert",
			stage: db2contract.StageEnrollmentInsert,
			// Twice, for the conflict path: the guard's three conditions only
			// run there, and the second call is an idempotent re-enrolment,
			// which must still answer a row.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEnrollmentInsertRequest(
					"kb", "live-probe-fingerprint", "CN=live-probe", "01ab",
					"2027-01-01T00:00:00Z", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, enrollmentID, err :=
					db2contract.DecodeEnrollmentInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 || enrollmentID == 0 {
					t.Fatal("the enrolment did not land")
				}
			},
		},
		{
			name:  "artifact_write_evidence",
			stage: db2contract.StageArtifactWriteEvidence,
			// Twice, because the second call is the one that finds the first by
			// its content hash and collapses onto it.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactWriteEvidenceRequest(
					"session_evidence", "project", liveProbeScopeProject,
					"live-probe", "live-probe-hash", `{"turns":4}`)
			},
			decoded: func(t *testing.T, body []byte) {
				artifactID, err := db2contract.DecodeArtifactWriteEvidenceReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if artifactID == "" {
					t.Fatal("no evidence artifact was written")
				}
			},
		},
		{
			name:  "bandit_decision_insert",
			stage: db2contract.StageBanditDecisionInsert,
			// A boolean parameter where the C binds a double, so this is the
			// probe that says the column accepts what pgx sends.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeBanditDecisionInsertRequest(
					"live-probe-decision", "recall", "arm-a", "ctx", 0.25, 1)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeBanditDecisionInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the decision was not recorded")
				}
			},
		},
		{
			name:  "calibration_profile_write",
			stage: db2contract.StageCalibrationProfileWrite,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCalibrationProfileWriteRequest(
					"recall", "synthesis", "project", liveProbeScopeProject,
					"v3", `{"bins":10}`)
			},
			decoded: func(t *testing.T, body []byte) {
				artifactID, err := db2contract.DecodeCalibrationProfileWriteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if artifactID == "" {
					t.Fatal("no calibration profile was written")
				}
			},
		},
		{
			name:  "feature_row_upsert",
			stage: db2contract.StageFeatureRowUpsert,
			// Twice, for the conflict branch: the compound conflict target is
			// only exercised there, and it names three columns.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeFeatureRowUpsertRequest(
					"memory:900012", "memory", "project", liveProbeScopeProject,
					"v3", `{"uses":9}`, "")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeFeatureRowUpsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the feature row was not written")
				}
			},
		},
		{
			name:  "learning_proposal_insert",
			stage: db2contract.StageLearningProposalInsert,
			// signal_id is a foreign key into learning_signals, so the signal
			// has to exist for the proposal to land.
			seed: []string{liveProbeLearningSignal},
			encode: func() ([]byte, error) {
				return db2contract.EncodeLearningProposalInsertRequest(
					900026, "rules", "build-state", 0, `{"op":"reinforce"}`, "", "")
			},
			decoded: func(t *testing.T, body []byte) {
				proposalID, err := db2contract.DecodeLearningProposalInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if proposalID == 0 {
					t.Fatal("no proposal was written")
				}
			},
		},
		{
			name:  "entity_edge_upsert",
			stage: db2contract.StageEntityEdgeUpsert,
			// Twice: the first call inserts and the second takes whichever
			// repeat path the database supports. Which path that is depends on
			// whether the unique index exists, and the probe caches its answer
			// per process -- the unit tests drive that cache with a fake, so it
			// is reset here to make this probe ask the real database.
			repeat: 2,
			encode: func() ([]byte, error) {
				entityEdgeIndexOnce = sync.Once{}
				return db2contract.EncodeEntityEdgeUpsertRequest(
					"live-probe-source", "depends_on", "live-probe-target", 0, 12, 99, 99)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, _, err := db2contract.DecodeEntityEdgeUpsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the edge was not recorded")
				}
			},
		},
		{
			name:  "agent_outcome_record",
			stage: db2contract.StageAgentOutcomeRecord,
			encode: func() ([]byte, error) {
				return db2contract.EncodeAgentOutcomeRecordRequest(
					"live-probe", "review", "succeeded", "", 4, 12, 90000, "")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeAgentOutcomeRecordReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the outcome was not recorded")
				}
			},
		},
		{
			name:  "artifact_write",
			stage: db2contract.StageArtifactWrite,
			// Twice, for the conflict path: the identifier is the caller's, so
			// a retry has to collapse rather than fail.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactWriteRequest(
					"live-probe-written", "synthesis", "proposed", "project",
					liveProbeScopeProject, "live-probe", 0.8, `{"claim":"x"}`)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeArtifactWriteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the artifact was not written")
				}
			},
		},
		{
			name:  "curator_invalidate_doc",
			stage: db2contract.StageCuratorInvalidateDoc,
			// Four chained CTEs, one of them an UPDATE feeding an INSERT that
			// builds two JSON objects per row. Whether PostgreSQL accepts that
			// shape -- and whether gen_random_uuid is there -- is the whole
			// question, and only a real database answers it.
			//
			// A document and a citing artifact are seeded so the statement has
			// something to stale: against an empty schema it would parse and
			// invalidate nothing, which is the same reply a broken join gives.
			seed: []string{
				liveProbeUpsertProject, liveProbeCuratorDoc,
				liveProbeRejectableArtifact, liveProbeCuratorCitation,
			},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCuratorInvalidateDocRequest(
					"live-probe-upsert-project", "docs/live-probe.md")
			},
			decoded: func(t *testing.T, body []byte) {
				invalidated, err := db2contract.DecodeCuratorInvalidateDocReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if invalidated != 1 {
					t.Fatalf("invalidated %d artifacts, want the seeded one", invalidated)
				}
			},
		},
		{
			name:  "entity_node_upsert",
			stage: db2contract.StageEntityNodeUpsert,
			// Twice, for the conflict branch: nine columns move there and the
			// key does not.
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityNodeUpsertRequest(
					"node:live-probe-upsert", 1, liveProbeScopeProject, "main",
					"src/live-probe.c:main", "src/live-probe.c", "main", "scan", 7)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeEntityNodeUpsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the node was not recorded")
				}
			},
		},
		{
			name:  "enrollment_revoke",
			stage: db2contract.StageEnrollmentRevoke,
			// Twice, because the second revocation is the one that must not move
			// the stamp the first set -- and only a real NULLIF over a real row
			// shows that.
			repeat: 2,
			seed:   []string{liveProbeEnrollment},
			encode: func() ([]byte, error) {
				return db2contract.EncodeEnrollmentRevokeRequest(900028)
			},
			decoded: func(t *testing.T, body []byte) {
				revoked, _, _, _, state, _, _, _, revokedAt, _, _, err :=
					db2contract.DecodeEnrollmentRevokeReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if revoked != 1 || state != "revoked" || revokedAt == "" {
					t.Fatalf("revoked = %d, state = %q, at = %q", revoked, state, revokedAt)
				}
			},
		},
		{
			name:   "artifact_write_ex",
			stage:  db2contract.StageArtifactWriteEx,
			repeat: 2,
			encode: func() ([]byte, error) {
				return db2contract.EncodeArtifactWriteExRequest(
					"live-probe-write-ex", "synthesis", "proposed", "project",
					liveProbeScopeProject, "live-probe", 0.8, 3, `{"claim":"x"}`)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeArtifactWriteExReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the artifact was not written")
				}
			},
		},
		{
			name:  "audit_event_write",
			stage: db2contract.StageAuditEventWrite,
			// The event carries a foreign key into artifacts, so the artifact
			// has to exist. Twice, for the conflict path -- the identifier is
			// the caller's and a retry must collapse.
			repeat: 2,
			seed:   []string{liveProbeRejectableArtifact},
			encode: func() ([]byte, error) {
				return db2contract.EncodeAuditEventWriteRequest(
					"live-probe-audit", "live-probe-artifact", "recall",
					"memory:4", "live-probe", "project", liveProbeScopeProject,
					0.9, 1, "", `{"state":"committed"}`)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeAuditEventWriteReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the audit event was not written")
				}
			},
		},
		{
			name:  "css_migration_enumerate",
			stage: db2contract.StageCssMigrationEnumerate,
			// Twice: the second run is the ON CONFLICT branch, which is the
			// whole point of the operation -- a re-enumerate refreshes
			// coverage rather than failing on what it wrote last time.
			repeat: 2,
			seed:   []string{liveProbeProject, liveProbeStyleGraph},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCssMigrationEnumerateRequest(liveProbeScopeProject)
			},
			decoded: func(t *testing.T, body []byte) {
				enumerated, err := db2contract.DecodeCssMigrationEnumerateReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if enumerated != 1 {
					t.Fatalf("enumerated = %d, want the seeded unit", enumerated)
				}
			},
		},
		{
			name:  "css_migration_assert_conventions",
			stage: db2contract.StageCssMigrationAssertConventions,
			// Twice, because the second call is the one that finds a prior
			// fact: it takes it FOR UPDATE inside the CTE and skips the
			// insert, and neither branch has ever run until it does.
			repeat: 2,
			seed:   []string{liveProbeProject, liveProbeStyleGraph},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCssMigrationAssertConventionsRequest(
					liveProbeScopeProject, "2026-01-01T00:00:00Z")
			},
			decoded: func(t *testing.T, body []byte) {
				asserted, err := db2contract.DecodeCssMigrationAssertConventionsReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if asserted != 2 {
					t.Fatalf("asserted = %d, want both conventions", asserted)
				}
			},
		},
		{
			name:  "css_render_snapshot_store",
			stage: db2contract.StageCssRenderSnapshotStore,
			// Twice for the upsert branch that replaces the C's delete and
			// insert.
			repeat: 2,
			seed:   []string{liveProbeProject},
			encode: func() ([]byte, error) {
				return db2contract.EncodeCssRenderSnapshotStoreRequest(
					liveProbeScopeProject, "src/app/button.css", "before",
					`{"color":"rgb(0, 0, 0)"}`, "")
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeCssRenderSnapshotStoreReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("the snapshot was not stored")
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

// A stylesheet the CSS probes can find: one component file in the project's
// current generation, one rule whose selector carries BEM's delimiters, one
// custom-property declaration, and one resolved class token.
//
// All four matter. Without the file the joins match nothing; without the
// delimiters and the custom property the convention assert derives its two
// findings from absences and never exercises the positive spellings; and
// without the class token the enumerate has no unit to record.
const liveProbeStyleGraph = `WITH styled_file AS (
 INSERT INTO files (id, project_id, generation, path, scanned_at)
 SELECT 900030, p.id, p.current_generation, 'src/app/button.css',
 '2026-01-01 00:00:00'
 FROM projects p WHERE p.name = 'live-probe-project' RETURNING id),
 styled_rule AS (
 INSERT INTO css_rules (id, file_id, selector)
 SELECT 900031, styled_file.id, '.card__title--wide' FROM styled_file
 RETURNING id),
 token_declaration AS (
 INSERT INTO css_declarations (id, rule_id, property, value)
 SELECT 900032, styled_rule.id, '--brand', '#ffffff' FROM styled_rule
 RETURNING id)
 INSERT INTO css_component_styles (id, component_file_id, class_token, resolved)
 SELECT 900033, styled_file.id, 'card__title', 1 FROM styled_file`

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

// A second artifact for the link probe, and a task for the state probe.
const (
	liveProbeArtifactTargetID = "live-probe-artifact-target"
	liveProbeArtifactTarget   = `INSERT INTO artifacts (id, kind, state, payload)
 VALUES ('live-probe-artifact-target', 'live-probe', 'committed', '{}'::jsonb)`
	liveProbeTaskID = 900010
	liveProbeTask   = `INSERT INTO tasks (id, title, state, created_at, updated_at)
 VALUES (900010, 'live probe task', 'open', '2026-01-01 00:00:00', '2026-01-01 00:00:00')`
)

// A current project for the file upsert, which takes its generation from one.
const (
	liveProbeUpsertProjectID = 900011
	liveProbeUpsertProject   = `INSERT INTO projects
 (id, name, root, scanned_at, lifecycle_state, current_generation)
 VALUES (900011, 'live-probe-upsert-project', '/live-probe',
 '2026-01-01 00:00:00', 'current', 1)`
)

// A memory for the alias and card probes, both of which carry a foreign key
// into memories.
const (
	liveProbeAliasMemoryID = 900012
	liveProbeAliasMemory   = `INSERT INTO memories
 (id, tier, kind, key, content, created_at, updated_at)
 VALUES (900012, 'L2', 'fact', 'live-probe-alias-memory', 'live probe',
 '2026-01-01 00:00:00', '2026-01-01 00:00:00')`
)

// An open contradiction directive naming a pair of memories, seeded so the
// resolve probe can ask for it in the reverse order.
const (
	liveProbeContradictionA = 900013
	liveProbeContradictionB = 900014
	liveProbeContradiction  = `INSERT INTO epistemic_directives
 (id, question, cause, state, memory_a_id, memory_b_id, created_at, updated_at)
 VALUES (900015, 'which is true?', 'contradiction', 'open', 900013, 900014,
 '2026-01-01 00:00:00', '2026-01-01 00:00:00')`
)

// A second memory, for the link probe's far end.
const (
	liveProbeLinkTargetID = 900016
	liveProbeLinkTarget   = `INSERT INTO memories
 (id, tier, kind, key, content, created_at, updated_at)
 VALUES (900016, 'L2', 'fact', 'live-probe-link-target', 'live probe',
 '2026-01-01 00:00:00', '2026-01-01 00:00:00')`
)

// A fence this purge holds, so the heartbeat and clear probes take the matched
// branch rather than the "not ours" one. The value is the identity the fence
// stores: the generation and the purge identifier, space separated.
const (
	liveProbeFenceProject    = "live-probe-purge-project"
	liveProbeFenceGeneration = "live-probe-generation"
	liveProbeFencePurgeID    = "live-probe-purge"
	liveProbeFence           = `INSERT INTO kb_runtime_state (state_key, state_value)
 VALUES ('project_purging:live-probe-purge-project',
 'live-probe-generation live-probe-purge')
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`
)

// A queued ingest job and a document awaiting review, for the two KB write
// probes. Both operations acknowledge a statement that matched nothing, so
// without a row to act on neither probe would prove anything about the write.
const (
	liveProbeIngestJob = `INSERT INTO kb_ingest_queue
 (id, project, root_path, status)
 VALUES (900017, 'live-probe-project', '/live-probe', 'running')`
	liveProbeReviewDoc = `INSERT INTO docs
 (id, content_hash, filename, state, review_needed, review_reason,
 created_at, updated_at)
 VALUES (900018, 'live-probe-hash', 'live-probe.md', 'staged', true, 'seeded',
 '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z')`
)

// Two tasks for the edge probe's ends, and the cross-repo meta row the blocked
// symbols recompute bumps. The meta row is a singleton keyed on id = 1, so the
// seed upserts rather than inserting: the replay schema may already carry one.
const (
	liveProbeEdgeSource = `INSERT INTO tasks
 (id, title, state, session_id, created_at, updated_at)
 VALUES (900019, 'live probe source', 'todo', 'live-probe-session',
 '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z')`
	liveProbeEdgeTarget = `INSERT INTO tasks
 (id, title, state, session_id, created_at, updated_at)
 VALUES (900020, 'live probe target', 'todo', 'live-probe-session',
 '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z')`
	liveProbeCrossRepoMeta = `INSERT INTO cross_repo_meta (id) VALUES (1)
 ON CONFLICT (id) DO NOTHING`
)

// The node an alias points at. Aliases carry a foreign key into entity_nodes,
// so without it the insert fails and the probe would be proving that a broken
// write is reported as broken.
const liveProbeEntityNode = `INSERT INTO entity_nodes (node_key, display_name)
 VALUES ('node:live-probe', 'live probe')
 ON CONFLICT (node_key) DO NOTHING`

// A pending ingest job for the claim probe, and an artifact for the rejection
// probe to reject. The audit event carries a foreign key into artifacts, so
// without the second the rejection's own audit half fails.
const (
	liveProbePendingIngestJob = `INSERT INTO kb_ingest_queue
 (id, project, root_path, status, priority)
 VALUES (900023, 'live-probe-project', '/live-probe', 'pending', 100)`
	liveProbeRejectableArtifact = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, payload)
 VALUES ('live-probe-artifact', 'synthesis', 'proposed', 'project',
 'live-probe-project', '{}')
 ON CONFLICT (id) DO NOTHING`
)

// A project for the trust probe to move. Trust is a column on projects, and
// the write locks the row before deciding, so without a row there is nothing to
// lock and nothing to change.
const liveProbeTrustProject = `INSERT INTO projects
 (id, name, root, scanned_at, lifecycle_state, current_generation, trust)
 VALUES (900025, 'live-probe-trust-project', '/live-probe',
 '2026-01-01T00:00:00Z', 'current', 1, 'untrusted')`

// A learning signal for the proposal probe's foreign key.
const liveProbeLearningSignal = `INSERT INTO learning_signals
 (id, signal_type, created_at)
 VALUES (900026, 'live-probe', '2026-01-01T00:00:00Z')`

// A chunk of an ingested document and an artifact citing it, so the
// invalidation probe has something to stale.
const (
	liveProbeCuratorDoc = `INSERT INTO kb_documents
 (id, project, generation, file_path, file_hash, chunk_index, content)
 VALUES (900027, 'live-probe-upsert-project', 1, 'docs/live-probe.md',
 'live-probe-hash', 0, 'live probe')`
	liveProbeCuratorCitation = `INSERT INTO artifact_citations
 (artifact_id, source_kind, source_id, span_start, span_end)
 VALUES ('live-probe-artifact', 'kb_document', '900027', 0, 0)`
)

// An active enrolment for the revoke probe. Revoking nothing answers not-found,
// which would prove only that the statement parses.
const liveProbeEnrollment = `INSERT INTO kb_enrollments
 (id, scope, fingerprint, serial, cert_issuer, cert_serial_norm, state,
 authority_id)
 VALUES (900028, 'kb', 'live-probe-revoke-fingerprint', '01ab',
 'CN=live-probe', '01ab', 'active', '00112233445566778899aabbccddeeff')`

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
