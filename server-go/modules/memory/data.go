package memory

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

const (
	// EventData is the complete memory data surface.  C callers may encode and
	// decode this frame at the event-bus edge, but persistence and memory policy
	// stay in this Go process.
	EventData uint32 = 5895
	StageData uint32 = 7

	dataTimeout = 2 * time.Minute
	maxDataBody = 1 << 20
)

type DataRequest struct {
	Operation             string    `json:"operation"`
	Scope                 Scope     `json:"scope"`
	ID                    int64     `json:"id,omitempty"`
	IDs                   []int64   `json:"ids,omitempty"`
	Kind                  string    `json:"kind,omitempty"`
	Tier                  string    `json:"tier,omitempty"`
	Key                   string    `json:"key,omitempty"`
	Content               string    `json:"content,omitempty"`
	Query                 string    `json:"query,omitempty"`
	Confidence            *float64  `json:"confidence,omitempty"`
	Limit                 int       `json:"limit,omitempty"`
	Sensitivity           int       `json:"sensitivity,omitempty"`
	TurnRequestsSensitive bool      `json:"turn_requests_sensitive,omitempty"`
	Success               bool      `json:"success,omitempty"`
	Workspace             string    `json:"workspace,omitempty"`
	Project               string    `json:"project,omitempty"`
	IncludeAll            bool      `json:"include_all,omitempty"`
	SignalType            string    `json:"signal_type,omitempty"`
	Rule                  string    `json:"rule,omitempty"`
	SessionID             string    `json:"session_id,omitempty"`
	Client                string    `json:"client,omitempty"`
	Tool                  string    `json:"tool,omitempty"`
	Path                  string    `json:"path,omitempty"`
	Home                  string    `json:"home,omitempty"`
	Command               string    `json:"command,omitempty"`
	ProjectsRoot          string    `json:"projects_root,omitempty"`
	MemorySegment         string    `json:"memory_segment,omitempty"`
	ContentCapacity       int       `json:"content_capacity,omitempty"`
	Entity                string    `json:"entity,omitempty"`
	AsOf                  string    `json:"as_of,omitempty"`
	State                 string    `json:"state,omitempty"`
	TriggerText           string    `json:"trigger_text,omitempty"`
	ActionText            string    `json:"action_text,omitempty"`
	AnchorEntity          string    `json:"anchor_entity,omitempty"`
	AnchorFile            string    `json:"anchor_file,omitempty"`
	Recurrence            string    `json:"recurrence,omitempty"`
	ValidUntil            string    `json:"valid_until,omitempty"`
	Question              string    `json:"question,omitempty"`
	Topic                 string    `json:"topic,omitempty"`
	Cause                 string    `json:"cause,omitempty"`
	Priority              int       `json:"priority,omitempty"`
	MemoryAID             int64     `json:"memory_a_id,omitempty"`
	MemoryBID             int64     `json:"memory_b_id,omitempty"`
	ResolutionMemoryID    int64     `json:"resolution_memory_id,omitempty"`
	Evidence              string    `json:"evidence,omitempty"`
	Note                  string    `json:"note,omitempty"`
	Relation              string    `json:"relation,omitempty"`
	SourceID              int64     `json:"source_id,omitempty"`
	TargetID              int64     `json:"target_id,omitempty"`
	Resolution            string    `json:"resolution,omitempty"`
	Reason                string    `json:"reason,omitempty"`
	Details               string    `json:"details,omitempty"`
	LifecycleState        string    `json:"lifecycle_state,omitempty"`
	ArchiveReason         string    `json:"archive_reason,omitempty"`
	TTLDays               int       `json:"ttl_days,omitempty"`
	Days                  int       `json:"days,omitempty"`
	LimitTokens           int       `json:"limit_tokens,omitempty"`
	SessionStart          bool      `json:"session_start,omitempty"`
	BlockType             string    `json:"block_type,omitempty"`
	Level                 int       `json:"level,omitempty"`
	SubjectKind           int       `json:"subject_kind,omitempty"`
	RelationCode          int       `json:"relation_code,omitempty"`
	ObjectKind            int       `json:"object_kind,omitempty"`
	MinVersions           int       `json:"min_versions,omitempty"`
	ArtifactType          string    `json:"artifact_type,omitempty"`
	ArtifactRef           string    `json:"artifact_ref,omitempty"`
	ArtifactHash          string    `json:"artifact_hash,omitempty"`
	Actor                 string    `json:"actor,omitempty"`
	Mode                  string    `json:"mode,omitempty"`
	Pattern               string    `json:"pattern,omitempty"`
	EpistemicKind         string    `json:"epistemic_kind,omitempty"`
	UseCases              string    `json:"use_cases,omitempty"`
	Authority             int       `json:"authority,omitempty"`
	AfterID               int64     `json:"after_id,omitempty"`
	Modes                 uint32    `json:"modes,omitempty"`
	Force                 bool      `json:"force,omitempty"`
	DryRun                bool      `json:"dry_run,omitempty"`
	Clusters              []string  `json:"clusters,omitempty"`
	Directories           []string  `json:"directories,omitempty"`
	Vector                []float64 `json:"vector,omitempty"`
	RecordType            string    `json:"record_type,omitempty"`
	Version               string    `json:"version,omitempty"`
	HitThreshold          int       `json:"hit_threshold,omitempty"`
	Dimension             int       `json:"dimension,omitempty"`
	MaxResults            int       `json:"max_results,omitempty"`
}

type Record struct {
	ID         int64   `json:"id"`
	Scope      Scope   `json:"scope"`
	Tier       string  `json:"tier"`
	Kind       string  `json:"kind"`
	Key        string  `json:"key"`
	Content    string  `json:"content"`
	Confidence float64 `json:"confidence"`
}

type DataResponse struct {
	Records            []Record             `json:"records"`
	Deleted            bool                 `json:"deleted,omitempty"`
	Allowed            *bool                `json:"allowed,omitempty"`
	Promoted           int                  `json:"promoted"`
	Demoted            int                  `json:"demoted"`
	Expired            int                  `json:"expired"`
	Skip               bool                 `json:"skip,omitempty"`
	Enforced           bool                 `json:"enforced,omitempty"`
	Reason             string               `json:"reason,omitempty"`
	Verdict            string               `json:"verdict,omitempty"`
	Name               string               `json:"name,omitempty"`
	SensitiveStatus    int                  `json:"sensitive_status"`
	Redacted           string               `json:"redacted,omitempty"`
	Ephemeral          bool                 `json:"ephemeral,omitempty"`
	Evidence           bool                 `json:"evidence,omitempty"`
	Classification     string               `json:"classification,omitempty"`
	Armed              int                  `json:"armed,omitempty"`
	Triggered          int                  `json:"triggered,omitempty"`
	Completed          int                  `json:"completed,omitempty"`
	ProspectiveExpired int                  `json:"prospective_expired,omitempty"`
	Block              *string              `json:"block,omitempty"`
	Count              *int                 `json:"count,omitempty"`
	ValidAt            *bool                `json:"valid_at,omitempty"`
	Updated            bool                 `json:"updated,omitempty"`
	Prospectives       []Prospective        `json:"prospectives,omitempty"`
	Directives         []Directive          `json:"directives,omitempty"`
	DirectiveCounts    *DirectiveCounts     `json:"directive_counts,omitempty"`
	Links              []MemoryLink         `json:"links,omitempty"`
	Provenance         []Provenance         `json:"provenance,omitempty"`
	Conflicts          []Conflict           `json:"conflicts,omitempty"`
	Scopes             []ScopeTag           `json:"scopes,omitempty"`
	ScopeRanks         []ScopeRank          `json:"scope_ranks,omitempty"`
	Stats              *MemoryStats         `json:"stats,omitempty"`
	Health             *MemoryHealth        `json:"health,omitempty"`
	Lifecycle          *LifecycleCounts     `json:"lifecycle,omitempty"`
	LifecycleState     string               `json:"lifecycle_state,omitempty"`
	Episodes           []Episode            `json:"episodes,omitempty"`
	Relations          []Relation           `json:"relations,omitempty"`
	EntityProfile      *EntityProfile       `json:"entity_profile,omitempty"`
	Payload            json.RawMessage      `json:"payload,omitempty"`
	Diagnostics        []Diagnostic         `json:"diagnostics,omitempty"`
	Answer             *AnswerResult        `json:"answer,omitempty"`
	Code               *int                 `json:"code,omitempty"`
	Rules              []OntologyRule       `json:"rules,omitempty"`
	IDs                []int64              `json:"ids,omitempty"`
	LowEffectiveness   []LowEffectiveness   `json:"low_effectiveness,omitempty"`
	SupersededKeys     []SupersededKey      `json:"superseded_keys,omitempty"`
	Reviews            []ReviewRecord       `json:"reviews"`
	Summaries          []MemorySummary      `json:"summaries,omitempty"`
	Scenes             []MemoryScene        `json:"scenes,omitempty"`
	SceneMembers       []SceneMember        `json:"scene_members,omitempty"`
	TierKindCounts     []TierKindCount      `json:"tier_kind_counts,omitempty"`
	Effectiveness      *EffectivenessStats  `json:"effectiveness,omitempty"`
	LintIssues         []LintIssue          `json:"lint_issues,omitempty"`
	Maintenance        *MaintenanceSummary  `json:"maintenance,omitempty"`
	ExportRecords      []ExportRecord       `json:"export_records,omitempty"`
	Metrics            *RuntimeMetrics      `json:"metrics,omitempty"`
	RecallRejections   []RecallRejection    `json:"recall_rejections,omitempty"`
	LegacyResults      []LegacySearchResult `json:"legacy_results,omitempty"`
	VectorHits         []VectorHit          `json:"vector_hits,omitempty"`
	Drift              *DriftResult         `json:"drift,omitempty"`
	SummaryCount       int                  `json:"summary_count,omitempty"`
	FactCount          int                  `json:"fact_count,omitempty"`
	Failed             int                  `json:"failed,omitempty"`
	FactWork           *MemoryFactWork      `json:"fact_work,omitempty"`
	FactCandidates     []FactCandidate      `json:"fact_candidates,omitempty"`
}

// recallGateDecision owns the inexpensive turn-level recall policy. Keeping it
// here makes the C gateway an event-bus adapter instead of a second memory
// implementation. The decision is deliberately conservative and fails open.
func recallGateDecision(query string) (skip bool, reason string) {
	query = strings.TrimSpace(query)
	if query == "" || len(query) > 64 {
		return false, ""
	}
	for i, r := range query {
		if r >= 0x80 || strings.ContainsRune("?/.-_:", r) || (r >= '0' && r <= '9') ||
			(r >= 'A' && r <= 'Z' && i > 0) {
			return false, ""
		}
	}
	lower := strings.ToLower(query)
	for _, word := range []string{"what", "why", "how", "when", "where", "who", "which", "does", "did", "is", "are", "can", "should", "would", "explain"} {
		if strings.Contains(lower, word) {
			return false, ""
		}
	}
	for _, prefix := range []string{"thanks", "thank", "ok", "okay", "got it", "great", "perfect", "nice", "cool", "yes", "no", "yep", "nope", "sure", "done", "ship it", "lgtm", "sounds good"} {
		if strings.HasPrefix(lower, prefix) {
			return true, "acknowledgement"
		}
	}
	return false, ""
}

func recallGateMode() (enabled, enforce bool) {
	switch strings.ToLower(strings.TrimSpace(os.Getenv("AIMEE_MEMORY_RECALL_GATE"))) {
	case "0", "off", "false", "no":
		return false, false
	case "enforce":
		return true, true
	default:
		return true, false
	}
}

type dataAdvancedStore interface {
	Supersede(context.Context, Scope, int64, string, float64) (Record, error)
	Feedback(context.Context, Scope, []int64, bool) error
	Maintenance(context.Context, Scope) (int, int, int, error)
}

type prospectiveStore interface {
	ProspectiveCounts(context.Context) (int, int, int, int, error)
}

type factRecallStore interface {
	RecallFacts(context.Context, string, string, bool, int) (string, int, error)
}

type temporalStore interface {
	ValidAt(context.Context, int64, string) (bool, error)
}

type prospectiveDataStore interface {
	ProspectiveCreate(context.Context, DataRequest) (Prospective, error)
	ProspectiveList(context.Context, string, int) ([]Prospective, error)
	ProspectiveComplete(context.Context, int64) (bool, error)
	ProspectiveSweep(context.Context) (int, error)
	ProspectiveMatch(context.Context, string, string, string, int) ([]Prospective, error)
	ProspectiveMarkTriggered(context.Context, int64) (bool, error)
	ProspectiveGet(context.Context, int64) (Prospective, error)
}

type directiveDataStore interface {
	DirectiveCreate(context.Context, DataRequest) (Directive, error)
	DirectiveList(context.Context, string, string, int) ([]Directive, error)
	DirectiveResolve(context.Context, int64, int64, string) (bool, error)
	DirectiveSuppress(context.Context, int64) (bool, error)
	DirectiveSweep(context.Context) (int, error)
	DirectiveMatch(context.Context, string, string, string, int) ([]Directive, error)
	DirectiveMarkSurfaced(context.Context, int64) (bool, error)
	DirectiveCounts(context.Context) (DirectiveCounts, error)
	DirectiveGet(context.Context, int64) (Directive, error)
}

type domainDataStore interface {
	Touch(context.Context, []int64) (int, error)
	UpdateContent(context.Context, int64, string) (bool, error)
	Reject(context.Context, int64, string) (bool, error)
	LinkCreate(context.Context, int64, int64, string) (MemoryLink, error)
	LinkQuery(context.Context, int64, int) ([]MemoryLink, error)
	LinkDelete(context.Context, int64) (bool, error)
	ProvenanceList(context.Context, int64, int) ([]Provenance, error)
	ProvenanceAdd(context.Context, int64, string, string, string) (Provenance, error)
	ConflictList(context.Context, int) ([]Conflict, error)
	ConflictRecord(context.Context, int64, int64) (Conflict, error)
	ConflictResolve(context.Context, int64, string) (bool, error)
	ScopeTag(context.Context, int64, Scope) (bool, error)
	ScopeCollect(context.Context, int64) ([]ScopeTag, error)
	PrimaryScope(context.Context, int64) (ScopeTag, error)
	ScopeRanks(context.Context, []int64, string, string, bool) ([]ScopeRank, error)
	Stats(context.Context) (MemoryStats, error)
	Health(context.Context) (MemoryHealth, error)
	LifecycleGet(context.Context, int64) (string, error)
	LifecycleTransition(context.Context, int64, string, string) (bool, error)
	LifecyclePending(context.Context, int64, int) (bool, error)
	LifecycleSweep(context.Context) (int, error)
	LifecycleCounts(context.Context) (LifecycleCounts, error)
	EpisodeList(context.Context, string, int) ([]Episode, error)
	EpisodeGet(context.Context, string) (Episode, error)
	RelationSearch(context.Context, string, string, int) ([]Relation, error)
	EntityEdges(context.Context, string, int) ([]Relation, error)
	EntityProfile(context.Context, string) (EntityProfile, error)
	FactHistory(context.Context, string, int) ([]Record, error)
}

type retrievalDataStore interface {
	RecallBundle(context.Context, string, int, bool) (json.RawMessage, error)
	BriefingBundle(context.Context, int) (json.RawMessage, error)
	AlertsBundle(context.Context, string) (json.RawMessage, error)
	AssembleContext(context.Context, Scope, string, string, int) (string, error)
	Diagnose(context.Context, Scope, string, int) ([]Diagnostic, error)
	Explain(context.Context, Scope, string, int64) (Diagnostic, error)
	Ask(context.Context, Scope, string, int) (AnswerResult, error)
}

type queryDataStore interface {
	KeyExists(context.Context, string) (bool, error)
	FindID(context.Context, string, string) (int64, error)
	QueryRecords(context.Context, string, string, int, int) ([]Record, error)
	LowEffectiveness(context.Context, float64, int) ([]LowEffectiveness, error)
	UnusedL2(context.Context, int, int) ([]Record, error)
	SupersededKeys(context.Context, int, int) ([]SupersededKey, error)
	ReviewList(context.Context, string, int) ([]ReviewRecord, error)
	Restore(context.Context, int64, string) (bool, error)
	SetArtifact(context.Context, int64, string, string, string) (bool, error)
	Summaries(context.Context, int64, int) ([]MemorySummary, error)
	Scenes(context.Context, int) ([]MemoryScene, error)
	SceneMembers(context.Context, int64, int) ([]SceneMember, error)
	AllIDs(context.Context) ([]int64, error)
	EpistemicKind(context.Context, int64) (string, error)
	DemoteConfidence(context.Context, int64) (bool, error)
	TierKindCounts(context.Context, int) ([]TierKindCount, error)
}

type mutationDataStore interface {
	InsertEpistemic(context.Context, DataRequest) (Record, error)
	UpdateAs(context.Context, int64, string, int) (int, int64, error)
	DeleteAs(context.Context, int64, int) (bool, error)
}

type maintenanceDataStore interface {
	EffectivenessStats(context.Context) (EffectivenessStats, error)
	Lint(context.Context, int) ([]LintIssue, error)
	RunMaintenance(context.Context, uint32, bool, bool) (MaintenanceSummary, error)
}

type exportDataStore interface {
	ExportRecords(context.Context, int64, int) ([]ExportRecord, error)
	ExportDecisionsJSONL(context.Context, string) (int, error)
}

type sessionDataStore interface {
	FoldSession(context.Context, string) (int, string, error)
}

type embeddingDataStore interface {
	UpsertEmbedding(context.Context, Record, []float32) error
}

type legacyDataStore interface {
	RebuildDerivedIndexes(context.Context, int) (int, error)
	LegacySearch(context.Context, []string, int) ([]LegacySearchResult, error)
	CompactLegacy(context.Context) (int, int, error)
	ScanConversations(context.Context, []string) (int, error)
	CheckDrift(context.Context, int64, string, string) (DriftResult, error)
	ExtractAntiPatterns(context.Context, string) (int, error)
	EscalateAntiPatterns(context.Context, int) (int, error)
	LearnStyle(context.Context) (int, error)
	GenerateEpisodeCard(context.Context, string) (int64, error)
	VectorCollectionExists(context.Context) (bool, error)
	RecreateVectorCollection(context.Context, int) error
	SearchVectors(context.Context, []float64, string, string, string, bool, int) ([]VectorHit, error)
	RebuildVectorIndex(context.Context, string) (int, int, error)
	FailedEmbeddingIDs(context.Context, int) ([]int64, error)
	MarkEmbeddingFailure(context.Context, int64, string) error
}

type memoryFactDataStore interface {
	ClaimMemoryFact(context.Context) (*MemoryFactWork, error)
	ParseMemoryFacts(context.Context, int64, string) ([]FactCandidate, error)
	FinishMemoryFact(context.Context, int64, bool, string) error
}

type DataStore interface {
	Get(context.Context, Scope, int64) (Record, error)
	Search(context.Context, Scope, string, string, string, int) ([]Record, error)
	Put(context.Context, Scope, Record) (Record, error)
	Delete(context.Context, Scope, int64) (bool, error)
}

var ErrMemoryNotFound = errors.New("memory: record not found")

type postgresDataStore struct {
	db        store.Queryer
	placement Placement
}

func NewPostgresDataStore(db store.Queryer, placement Placement) (DataStore, error) {
	if db == nil {
		return nil, errors.New("memory: no postgres capability")
	}
	if placement != PlacementServer && placement != PlacementKB {
		return nil, fmt.Errorf("memory: invalid placement %q", placement)
	}
	return &postgresDataStore{db: db, placement: placement}, nil
}

func (s *postgresDataStore) Get(ctx context.Context, scope Scope, id int64) (Record, error) {
	return s.get(ctx, scope, id, false)
}

func (s *postgresDataStore) get(ctx context.Context, scope Scope, id int64, historical bool) (Record, error) {
	var r Record
	if s.placement == PlacementServer {
		r.Scope = scope
		err := s.db.QueryRow(ctx, `SELECT id, tier, kind, key, content, confidence
FROM user_memories
WHERE id = $1 AND lifecycle_state = 'active'
  AND (valid_until IS NULL OR valid_until > now())`, id).
			Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
		if store.IsNoRows(err) {
			return Record{}, ErrMemoryNotFound
		}
		return r, err
	}
	err := s.db.QueryRow(ctx, `SELECT id, scope_type, scope_value, tier, kind, key, content, confidence
FROM memories
WHERE id = $1 AND ($2 OR lifecycle_state='active')`, id, historical).
		Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
	if store.IsNoRows(err) {
		return Record{}, ErrMemoryNotFound
	}
	return r, err
}

func (s *postgresDataStore) UpsertEmbedding(ctx context.Context, record Record, vector []float32) error {
	if record.ID <= 0 || len(vector) == 0 {
		return errors.New("memory: invalid embedding record")
	}
	components := make([]string, len(vector))
	for i, component := range vector {
		components[i] = strconv.FormatFloat(float64(component), 'g', -1, 32)
	}
	vectorText := "[" + strings.Join(components, ",") + "]"
	primaryScope := record.Scope.Type
	if primaryScope == "" {
		primaryScope = "global"
	}
	workspace, project := "", ""
	if record.Scope.Type == "workspace" {
		workspace = record.Scope.Value
	}
	if record.Scope.Type == "project" {
		project = record.Scope.Value
	}
	payload, err := json.Marshal(map[string]any{
		"record_type": "memory", "memory_id": record.ID, "kind": record.Kind,
		"key": record.Key, "primary_scope": primaryScope,
		"workspace": workspace, "project": project,
	})
	if err != nil {
		return err
	}
	_, err = s.db.Exec(ctx, `INSERT INTO memory_embeddings
  (point_id, embedding, record_type, primary_scope, workspace, project, kind, payload_json)
VALUES ($1, $2::vector, 'memory', $3, $4, $5, $6, $7)
ON CONFLICT (point_id) DO UPDATE SET
  embedding = EXCLUDED.embedding, record_type = EXCLUDED.record_type,
  primary_scope = EXCLUDED.primary_scope, workspace = EXCLUDED.workspace,
  project = EXCLUDED.project, kind = EXCLUDED.kind, payload_json = EXCLUDED.payload_json`,
		record.ID, vectorText, primaryScope, workspace, project, record.Kind, string(payload))
	if err == nil {
		_, err = s.db.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,indexed_at,updated_at)
VALUES($1,'memory',$1,'indexed',0,'',pg_now_text(),pg_now_text()) ON CONFLICT(point_id) DO UPDATE SET
status='indexed',last_error='',indexed_at=pg_now_text(),updated_at=pg_now_text()`, record.ID)
	}
	return err
}

func (s *postgresDataStore) Supersede(ctx context.Context, scope Scope, id int64, content string, confidence float64) (Record, error) {
	if s.placement == PlacementServer {
		// Personal memory has one row per (kind,key). Replace atomically: a
		// failed write must not retire the only copy or touch the KB namespace.
		r := Record{Scope: scope}
		err := s.db.QueryRow(ctx, `UPDATE user_memories
SET content=$2, confidence=$3, updated_at=now()
WHERE id=$1 AND lifecycle_state='active'
  AND (valid_until IS NULL OR valid_until>now())
RETURNING id,tier,kind,key,content,confidence`, id, content, confidence).
			Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
		if store.IsNoRows(err) {
			return Record{}, ErrMemoryNotFound
		}
		return r, err
	}
	old, err := s.Get(ctx, scope, id)
	if err != nil {
		return Record{}, err
	}
	if old.Scope.Type != "" {
		scope = old.Scope
	}
	if _, err = s.Delete(ctx, scope, id); err != nil {
		return Record{}, err
	}
	old.ID, old.Content, old.Confidence = 0, content, confidence
	return s.Put(ctx, scope, old)
}

func (s *postgresDataStore) Feedback(ctx context.Context, scope Scope, ids []int64, success bool) error {
	if len(ids) == 0 || s.placement == PlacementServer {
		return nil
	}
	delta := -0.1
	if success {
		delta = 0.1
	}
	for _, id := range ids {
		if id <= 0 {
			continue
		}
		_, err := s.db.Exec(ctx, `UPDATE entity_edges SET utility_score = GREATEST(-1.0, LEAST(1.0, COALESCE(utility_score, 0) + $1)), utility_touched_at = pg_now_text()
WHERE source = (SELECT key FROM memories WHERE id = $2)
   OR target = (SELECT key FROM memories WHERE id = $2)`, delta, id)
		if err != nil {
			return err
		}
	}
	return nil
}

func (s *postgresDataStore) Maintenance(ctx context.Context, scope Scope) (int, int, int, error) {
	table := "memories"
	where := "scope_type = $1 AND scope_value = $2 AND "
	args := []any{scope.Type, scope.Value}
	stamp := "pg_now_text()"
	expiryCutoff := "pg_now_text('-90 days')"
	if s.placement == PlacementServer {
		table, where, args = "user_memories", "", nil
		stamp, expiryCutoff = "now()", "now() - interval '90 days'"
	}
	promoteSQL := fmt.Sprintf("UPDATE %s SET tier = 'L3', updated_at = %s WHERE %stier = 'L2' AND confidence >= 0.95 AND use_count >= 5", table, stamp, where)
	demoteSQL := fmt.Sprintf("UPDATE %s SET tier = 'L1', updated_at = %s WHERE %stier = 'L2' AND confidence < 0.4", table, stamp, where)
	expireSQL := fmt.Sprintf("UPDATE %s SET lifecycle_state = 'retired', updated_at = %s WHERE %stier IN ('L0','L1') AND updated_at < %s AND lifecycle_state = 'active'", table, stamp, where, expiryCutoff)
	counts := [3]int{}
	for i, query := range []string{promoteSQL, demoteSQL, expireSQL} {
		tag, err := s.db.Exec(ctx, query, args...)
		if err != nil {
			return 0, 0, 0, err
		}
		counts[i] = int(tag.RowsAffected())
	}
	return counts[0], counts[1], counts[2], nil
}

func (s *postgresDataStore) ProspectiveCounts(ctx context.Context) (int, int, int, int, error) {
	if s.placement != PlacementKB {
		return 0, 0, 0, 0, errors.New("memory: prospective memory belongs to kb placement")
	}
	var armed, triggered, completed, expired int
	err := s.db.QueryRow(ctx, `SELECT
  COUNT(*) FILTER (WHERE state = 'armed'),
  COUNT(*) FILTER (WHERE state = 'triggered'),
  COUNT(*) FILTER (WHERE state = 'completed'),
  COUNT(*) FILTER (WHERE state = 'expired')
FROM prospective_memories`).Scan(&armed, &triggered, &completed, &expired)
	return armed, triggered, completed, expired, err
}

func (s *postgresDataStore) Search(ctx context.Context, scope Scope, query, kind, tier string, limit int) ([]Record, error) {
	pattern := searchPattern(query)
	var (
		rows store.Rows
		err  error
	)
	if s.placement == PlacementServer {
		rows, err = s.db.Query(ctx, `SELECT id, tier, kind, key, content, confidence
FROM user_memories
WHERE lifecycle_state = 'active'
  AND (valid_until IS NULL OR valid_until > now())
  AND ($5 = '' OR key ILIKE $1 OR content ILIKE $1
       OR to_tsvector('english', key || ' ' || content) @@ plainto_tsquery('english', $5))
  AND ($2 = '' OR kind = $2) AND ($3 = '' OR tier = $3)
ORDER BY (lower(key)=lower($5)) DESC,
  ts_rank_cd(to_tsvector('english', key || ' ' || content), plainto_tsquery('english', $5)) DESC,
  confidence DESC, updated_at DESC, id DESC LIMIT $4`, pattern, kind, tier, limit, query)
	} else {
		rows, err = s.db.Query(ctx, `SELECT id, scope_type, scope_value, tier, kind, key, content, confidence
FROM memories
WHERE lifecycle_state = 'active' AND scope_type = $1 AND scope_value = $2
  AND ($7 = '' OR key ILIKE $3 OR content ILIKE $3 OR use_cases ILIKE $3
       OR to_tsvector('english', key || ' ' || content || ' ' || COALESCE(use_cases,''))
          @@ plainto_tsquery('english', $7))
  AND ($4 = '' OR kind = $4) AND ($5 = '' OR tier = $5)
ORDER BY (lower(key)=lower($7)) DESC,
  ts_rank_cd(to_tsvector('english', key || ' ' || content || ' ' || COALESCE(use_cases,'')),
             plainto_tsquery('english', $7)) DESC,
  confidence DESC, updated_at DESC, id DESC LIMIT $6`,
			scope.Type, scope.Value, pattern, kind, tier, limit, query)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	records := make([]Record, 0)
	for rows.Next() {
		var r Record
		if s.placement == PlacementServer {
			r.Scope = scope
			err = rows.Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
		} else {
			err = rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind,
				&r.Key, &r.Content, &r.Confidence)
		}
		if err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return records, nil
}

// searchPattern keeps a multi-word query useful when callers supply keyword
// clusters rather than a literal phrase. PostgreSQL still receives one bound
// value, while the gaps between terms may contain arbitrary text.
func searchPattern(query string) string {
	terms := strings.Fields(query)
	if len(terms) == 0 {
		return "%"
	}
	return "%" + strings.Join(terms, "%") + "%"
}

func (s *postgresDataStore) Put(ctx context.Context, scope Scope, r Record) (Record, error) {
	if r.Tier == "" {
		r.Tier = "L2"
	}
	r.Scope = scope
	if s.placement == PlacementServer {
		err := s.db.QueryRow(ctx, `INSERT INTO user_memories
  (kind, tier, key, content, confidence, updated_at)
VALUES ($1, $2, $3, $4, $5, now())
ON CONFLICT (kind, key) DO UPDATE SET
  tier = EXCLUDED.tier, content = EXCLUDED.content,
  confidence = EXCLUDED.confidence, lifecycle_state = 'active',
  valid_until = NULL, updated_at = now()
RETURNING id`, r.Kind, r.Tier, r.Key, r.Content, r.Confidence).Scan(&r.ID)
		return r, err
	}
	err := s.db.QueryRow(ctx, `WITH updated AS (
  UPDATE memories SET tier = $2, content = $4, confidence = $5,
    updated_at = pg_now_text()
  WHERE kind = $1 AND key = $3 AND scope_type = $6 AND scope_value = $7
    AND lifecycle_state = 'active'
  RETURNING id
), inserted AS (
  INSERT INTO memories
    (kind, tier, key, content, confidence, scope_type, scope_value, lifecycle_state)
  SELECT $1, $2, $3, $4, $5, $6, $7, 'active'
  WHERE NOT EXISTS (SELECT 1 FROM updated)
  RETURNING id
)
SELECT id FROM updated UNION ALL SELECT id FROM inserted LIMIT 1`,
		r.Kind, r.Tier, r.Key, r.Content, r.Confidence, scope.Type, scope.Value).Scan(&r.ID)
	return r, err
}

func (s *postgresDataStore) Delete(ctx context.Context, scope Scope, id int64) (bool, error) {
	var (
		tag store.Tag
		err error
	)
	if s.placement == PlacementServer {
		tag, err = s.db.Exec(ctx, `UPDATE user_memories SET lifecycle_state = 'retired', updated_at = now()
WHERE id = $1 AND lifecycle_state = 'active'`, id)
	} else {
		tag, err = s.db.Exec(ctx, `UPDATE memories SET lifecycle_state = 'retired', updated_at = pg_now_text()
WHERE id = $1 AND scope_type = $2 AND scope_value = $3 AND lifecycle_state = 'active'`,
			id, scope.Type, scope.Value)
	}
	if err != nil {
		return false, err
	}
	return tag.RowsAffected() > 0, nil
}

type handlerOptions struct {
	placement Placement
	data      DataStore
}

type HandlerOption func(*handlerOptions)

func WithDataStore(placement Placement, data DataStore) HandlerOption {
	return func(options *handlerOptions) {
		options.placement = placement
		options.data = data
	}
}

func decodeDataRequest(body []byte) (DataRequest, error) {
	if len(body) == 0 || len(body) > maxDataBody {
		return DataRequest{}, errors.New("memory: invalid data request size")
	}
	decoder := json.NewDecoder(bytes.NewReader(body))
	decoder.DisallowUnknownFields()
	var request DataRequest
	if err := decoder.Decode(&request); err != nil {
		return DataRequest{}, err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return DataRequest{}, errors.New("memory: trailing data request")
	}
	request.Operation = strings.ToLower(strings.TrimSpace(request.Operation))
	request.Kind = strings.TrimSpace(request.Kind)
	request.Tier = strings.TrimSpace(request.Tier)
	request.Key = strings.TrimSpace(request.Key)
	request.Workspace = strings.TrimSpace(request.Workspace)
	request.Project = strings.TrimSpace(request.Project)
	request.SignalType = strings.TrimSpace(request.SignalType)
	request.Client = strings.TrimSpace(request.Client)
	request.Tool = strings.TrimSpace(request.Tool)
	request.ProjectsRoot = strings.TrimSpace(request.ProjectsRoot)
	request.MemorySegment = strings.TrimSpace(request.MemorySegment)
	request.Entity = strings.TrimSpace(request.Entity)
	request.AsOf = strings.TrimSpace(request.AsOf)
	request.State = strings.ToLower(strings.TrimSpace(request.State))
	request.TriggerText = strings.TrimSpace(request.TriggerText)
	request.ActionText = strings.TrimSpace(request.ActionText)
	request.AnchorEntity = strings.TrimSpace(request.AnchorEntity)
	request.AnchorFile = strings.TrimSpace(request.AnchorFile)
	request.Recurrence = strings.ToLower(strings.TrimSpace(request.Recurrence))
	request.ValidUntil = strings.TrimSpace(request.ValidUntil)
	request.Question = strings.TrimSpace(request.Question)
	request.Topic = strings.TrimSpace(request.Topic)
	request.Cause = strings.ToLower(strings.TrimSpace(request.Cause))
	request.Evidence = strings.TrimSpace(request.Evidence)
	request.Note = strings.TrimSpace(request.Note)
	request.Relation = strings.TrimSpace(request.Relation)
	request.Resolution = strings.TrimSpace(request.Resolution)
	request.Reason = strings.TrimSpace(request.Reason)
	request.Details = strings.TrimSpace(request.Details)
	request.LifecycleState = strings.ToLower(strings.TrimSpace(request.LifecycleState))
	request.ArchiveReason = strings.TrimSpace(request.ArchiveReason)
	request.BlockType = strings.TrimSpace(request.BlockType)
	request.ArtifactType = strings.TrimSpace(request.ArtifactType)
	request.ArtifactRef = strings.TrimSpace(request.ArtifactRef)
	request.ArtifactHash = strings.TrimSpace(request.ArtifactHash)
	request.Actor = strings.TrimSpace(request.Actor)
	request.Mode = strings.TrimSpace(request.Mode)
	request.Pattern = strings.TrimSpace(request.Pattern)
	request.EpistemicKind = strings.TrimSpace(request.EpistemicKind)
	request.UseCases = strings.TrimSpace(request.UseCases)
	request.RecordType = strings.TrimSpace(request.RecordType)
	request.Version = strings.TrimSpace(request.Version)
	if request.Limit == 0 {
		request.Limit = 20
	}
	if request.Limit < 1 || request.Limit > 100 || len(request.Kind) > 64 ||
		len(request.Tier) > 16 || len(request.Key) > 4096 || len(request.Query) > 16384 ||
		len(request.Content) > 512*1024 || len(request.Workspace) > 1024 ||
		len(request.Project) > 1024 || len(request.SignalType) > 64 || len(request.Rule) > 512*1024 ||
		len(request.SessionID) > 256 || len(request.Client) > 64 || len(request.Tool) > 64 ||
		len(request.Path) > 4096 || len(request.Home) > 4096 || len(request.Command) > 65536 ||
		len(request.ProjectsRoot) > 4096 || len(request.MemorySegment) > 256 ||
		len(request.Entity) > 512 || len(request.AsOf) > 64 || len(request.State) > 16 ||
		len(request.TriggerText) > 511 || len(request.ActionText) > 1023 ||
		len(request.AnchorEntity) > 127 || len(request.AnchorFile) > 127 ||
		len(request.Recurrence) > 15 || len(request.ValidUntil) > 64 ||
		len(request.Question) > 511 || len(request.Topic) > 127 || len(request.Cause) > 31 ||
		len(request.Evidence) > 511 || len(request.Note) > 4096 || len(request.Relation) > 127 ||
		len(request.Resolution) > 255 || len(request.Reason) > 1024 || len(request.Details) > 4096 ||
		len(request.LifecycleState) > 31 || len(request.ArchiveReason) > 1024 ||
		len(request.BlockType) > 64 || request.LimitTokens < 0 || request.LimitTokens > 8192 ||
		len(request.ArtifactType) > 128 || len(request.ArtifactRef) > 4096 ||
		len(request.ArtifactHash) > 256 || len(request.Actor) > 256 ||
		len(request.Mode) > 64 || len(request.Pattern) > 16384 ||
		len(request.EpistemicKind) > 32 || len(request.UseCases) > 65536 ||
		len(request.RecordType) > 64 || len(request.Version) > 256 ||
		len(request.Clusters) > 64 || len(request.Directories) > 8 || len(request.Vector) > 4000 ||
		request.TTLDays < 0 || request.TTLDays > 36500 || request.Days < 0 || request.Days > 36500 ||
		request.AfterID < 0 ||
		request.Dimension < 0 || request.Dimension > 4000 ||
		request.MaxResults < 0 || request.MaxResults > 256 ||
		request.ContentCapacity < 0 || request.ContentCapacity > 512*1024 {
		return DataRequest{}, errors.New("memory: data request exceeds bounds")
	}
	if request.Confidence != nil && (*request.Confidence < 0 || *request.Confidence > 1) {
		return DataRequest{}, errors.New("memory: confidence must be between zero and one")
	}
	return request, nil
}

func handleData(options handlerOptions, invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	request, err := decodeDataRequest(body)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	explicitScope := request.Scope.Type != "" || request.Scope.Value != ""
	if options.placement == PlacementKB && !explicitScope {
		if request.Project != "" {
			request.Scope = Scope{Type: ScopeProject, Value: request.Project}
		} else if request.Workspace != "" {
			request.Scope = Scope{Type: ScopeWorkspace, Value: request.Workspace}
		}
	}
	scope, err := normalizeScope(options.placement, request.Scope)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	if request.Operation == "recall-gate" {
		enabled, enforce := recallGateMode()
		if enabled {
			response := DataResponse{}
			response.Skip, response.Reason = recallGateDecision(request.Query)
			response.Enforced = response.Skip && enforce
			encoded, marshalErr := json.Marshal(response)
			if marshalErr != nil {
				return nil, bus.ModuleStatusInternal
			}
			return encoded, bus.ModuleStatusOK
		}
		encoded, marshalErr := json.Marshal(DataResponse{})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "redirect-classify" {
		verdict := classifyRedirect(request.Client, request.Tool, request.Path, request.Home,
			request.ProjectsRoot, request.MemorySegment)
		encoded, marshalErr := json.Marshal(DataResponse{Verdict: verdict.Verdict,
			Name: verdict.Name, Reason: verdict.Reason})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "redirect-bash" {
		verdict := "allow"
		if bashTargetsMemory(request.Client, request.Command, request.Home,
			request.ProjectsRoot, request.MemorySegment) {
			verdict = "reject"
		}
		encoded, marshalErr := json.Marshal(DataResponse{Verdict: verdict})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "content-gate" {
		gate := scanContent(request.Content, request.ContentCapacity)
		encoded, marshalErr := json.Marshal(DataResponse{
			SensitiveStatus: gate.SensitiveStatus,
			Redacted:        gate.Redacted,
			Ephemeral:       gate.Ephemeral,
			Evidence:        gate.Evidence,
			Classification:  gate.Classification,
		})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "fusion-state-set" || request.Operation == "fusion-state-clear" ||
		request.Operation == "fusion-state-get" {
		switch request.Operation {
		case "fusion-state-set":
			setGraphFusion(request.State)
		case "fusion-state-clear":
			clearGraphFusion()
		}
		enabled := isGraphFusionEnabled()
		encoded, marshalErr := json.Marshal(DataResponse{Allowed: &enabled})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "directive-metrics" || request.Operation == "prospective-metrics" ||
		request.Operation == "recall-metrics" {
		var metrics RuntimeMetrics
		switch request.Operation {
		case "directive-metrics":
			metrics = directiveMetrics()
		case "prospective-metrics":
			metrics = prospectiveMetrics()
		case "recall-metrics":
			metrics = recallMetrics()
		}
		encoded, marshalErr := json.Marshal(DataResponse{Metrics: &metrics})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "recall-trace-begin" || request.Operation == "recall-trace-end" ||
		request.Operation == "recall-trace-list" {
		switch request.Operation {
		case "recall-trace-begin":
			recallTraceBegin()
		case "recall-trace-end":
			recallTraceEnd()
		}
		encoded, marshalErr := json.Marshal(DataResponse{RecallRejections: recallTraceSnapshot()})
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if request.Operation == "tier-name" || request.Operation == "scope-level-name" ||
		request.Operation == "ontology-relation-name" || request.Operation == "ontology-relation-code" ||
		request.Operation == "ontology-node-name" || request.Operation == "ontology-node-code" ||
		request.Operation == "ontology-validate" || request.Operation == "ontology-rules" {
		response := DataResponse{}
		switch request.Operation {
		case "tier-name":
			response.Name = functionalTierName(request.Tier)
		case "scope-level-name":
			response.Name = scopeLevelName(request.Level)
		case "ontology-relation-name":
			response.Name = relationName(request.RelationCode)
		case "ontology-relation-code":
			code := relationCode(request.Relation)
			response.Code = &code
		case "ontology-node-name":
			response.Name = nodeName(request.SubjectKind)
		case "ontology-node-code":
			code := nodeCode(request.Kind)
			response.Code = &code
		case "ontology-validate":
			allowed := ontologyValid(request.SubjectKind, request.RelationCode, request.ObjectKind)
			response.Allowed = &allowed
		case "ontology-rules":
			response.Rules = ontologyRules()
		}
		encoded, marshalErr := json.Marshal(response)
		if marshalErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
	if options.data == nil {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	timeout := invocation.Remaining(dataTimeout)
	if timeout <= 0 {
		return nil, bus.ModuleStatusCancelled
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	// Request scope used to live on the C connection. Pin it to the Go store
	// transaction now, so the non-owner runtime sees precisely this request's
	// rows and pooled connections cannot retain another request's scope.
	var transaction store.Tx
	if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementKB {
		if db, ok := backend.db.(store.DB); ok {
			transaction, err = db.Begin(ctx)
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			defer transaction.Rollback(context.Background())
			_, err = transaction.Exec(ctx, `SELECT
set_config('aimee.memory_scope_type',$1,true),
set_config('aimee.memory_scope_value',$2,true),
set_config('aimee.memory_workspace',$3,true),
set_config('aimee.memory_project',$4,true),
set_config('aimee.memory_scope_all',$5,true)`,
				string(scope.Type), scope.Value, request.Workspace, request.Project,
				map[bool]string{false: "0", true: "1"}[request.IncludeAll])
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			bound := *backend
			bound.db = transaction
			options.data = &bound
		}
	}

	response := DataResponse{}
	switch request.Operation {
	case "memory-facts-claim", "memory-facts-parse", "memory-facts-finish":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		facts, ok := options.data.(memoryFactDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "memory-facts-claim":
			response.FactWork, err = facts.ClaimMemoryFact(ctx)
		case "memory-facts-parse":
			if request.ID <= 0 || request.Content == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.FactCandidates, err = facts.ParseMemoryFacts(ctx, request.ID, request.Content)
		case "memory-facts-finish":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			err = facts.FinishMemoryFact(ctx, request.ID, request.Success, request.Reason)
			response.Updated = err == nil
		}
	case "rebuild-derived", "legacy-search", "compact-legacy", "scan-conversations", "check-drift",
		"anti-pattern-feedback", "anti-pattern-failures", "anti-pattern-escalate", "learn-style",
		"episode-card-generate", "vector-collection-exists", "vector-collection-recreate",
		"vector-search", "vector-rebuild":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		legacy, ok := options.data.(legacyDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "rebuild-derived":
			var count int
			count, err = legacy.RebuildDerivedIndexes(ctx, request.Limit)
			response.Count = &count
		case "legacy-search":
			response.LegacyResults, err = legacy.LegacySearch(ctx, request.Clusters, request.Limit)
		case "compact-legacy":
			response.SummaryCount, response.FactCount, err = legacy.CompactLegacy(ctx)
		case "scan-conversations":
			var count int
			count, err = legacy.ScanConversations(ctx, request.Directories)
			response.Count = &count
		case "check-drift":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var drift DriftResult
			drift, err = legacy.CheckDrift(ctx, request.ID, request.Path, request.Command)
			response.Drift = &drift
		case "anti-pattern-feedback":
			var count int
			count, err = legacy.ExtractAntiPatterns(ctx, "feedback")
			response.Count = &count
		case "anti-pattern-failures":
			var count int
			count, err = legacy.ExtractAntiPatterns(ctx, "failure")
			response.Count = &count
		case "anti-pattern-escalate":
			var count int
			count, err = legacy.EscalateAntiPatterns(ctx, request.HitThreshold)
			response.Count = &count
		case "learn-style":
			var count int
			count, err = legacy.LearnStyle(ctx)
			response.Count = &count
		case "episode-card-generate":
			var id int64
			id, err = legacy.GenerateEpisodeCard(ctx, request.SessionID)
			response.IDs = []int64{id}
		case "vector-collection-exists":
			var exists bool
			exists, err = legacy.VectorCollectionExists(ctx)
			response.Allowed = &exists
		case "vector-collection-recreate":
			err = legacy.RecreateVectorCollection(ctx, request.Dimension)
			response.Updated = err == nil
		case "vector-search":
			response.VectorHits, err = legacy.SearchVectors(ctx, request.Vector, request.RecordType,
				request.Workspace, request.Project, request.IncludeAll, request.MaxResults)
		case "vector-rebuild":
			var rebuilt int
			rebuilt, response.Failed, err = legacy.RebuildVectorIndex(ctx, request.Version)
			response.Count = &rebuilt
		}
	case "fold-session":
		if options.placement != PlacementKB || request.SessionID == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		sessions, ok := options.data.(sessionDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var count int
		var block string
		count, block, err = sessions.FoldSession(ctx, request.SessionID)
		response.Count, response.Block = &count, &block
	case "export-records", "export-decisions-jsonl":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		exporter, ok := options.data.(exportDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Operation == "export-records" {
			response.ExportRecords, err = exporter.ExportRecords(ctx, request.AfterID, request.Limit)
		} else {
			if request.Path == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var count int
			count, err = exporter.ExportDecisionsJSONL(ctx, request.Path)
			response.Count = &count
		}
	case "effectiveness-stats", "lint", "scheduled-maintenance":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		maintenance, ok := options.data.(maintenanceDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "effectiveness-stats":
			var stats EffectivenessStats
			stats, err = maintenance.EffectivenessStats(ctx)
			response.Effectiveness = &stats
		case "lint":
			response.LintIssues, err = maintenance.Lint(ctx, request.Limit)
		case "scheduled-maintenance":
			var summary MaintenanceSummary
			summary, err = maintenance.RunMaintenance(ctx, request.Modes, request.Force, request.DryRun)
			response.Maintenance = &summary
		}
	case "insert-epistemic", "update-as", "delete-as":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		mutations, ok := options.data.(mutationDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "insert-epistemic":
			if request.Tier == "" || request.Kind == "" || request.Key == "" || request.Content == "" ||
				(request.Authority != AuthorityModel && request.Authority != AuthorityUser) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			request.Scope = scope
			var record Record
			record, err = mutations.InsertEpistemic(ctx, request)
			response.Records = []Record{record}
		case "update-as":
			if request.ID <= 0 || request.Content == "" ||
				(request.Authority != AuthorityModel && request.Authority != AuthorityUser) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var code int
			var newID int64
			code, newID, err = mutations.UpdateAs(ctx, request.ID, request.Content, request.Authority)
			response.Code = &code
			response.IDs = []int64{newID}
		case "delete-as":
			if request.ID <= 0 || (request.Authority != AuthorityModel && request.Authority != AuthorityUser) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Deleted, err = mutations.DeleteAs(ctx, request.ID, request.Authority)
		}
	case "pii-inject":
		if request.Sensitivity < int(SensNormal) || request.Sensitivity > int(SensSecret) ||
			request.Confidence == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		allowed := ShouldInject(RelSensitivity(request.Sensitivity), *request.Confidence,
			request.TurnRequestsSensitive)
		response.Allowed = &allowed
	case "get":
		if request.ID <= 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var record Record
		var getErr error
		if request.AsOf != "" {
			if options.placement != PlacementKB {
				return nil, bus.ModuleStatusInvalidRequest
			}
			backend, ok := options.data.(*postgresDataStore)
			if !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			record, getErr = backend.get(ctx, scope, request.ID, true)
		} else {
			record, getErr = options.data.Get(ctx, scope, request.ID)
		}
		if errors.Is(getErr, ErrMemoryNotFound) {
			response.Records = []Record{}
			break
		}
		err = getErr
		response.Records = []Record{record}
	case "search", "recall", "briefing", "list":
		query := request.Query
		if request.Operation == "briefing" || request.Operation == "list" {
			query = ""
		}
		if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementKB && !explicitScope {
			request.Query = query
			response.Records, err = backend.SearchVisible(ctx, request)
		} else {
			response.Records, err = options.data.Search(ctx, scope, query, request.Kind, request.Tier, request.Limit)
		}
	case "visible-search":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if backend, ok := options.data.(*postgresDataStore); ok {
			response.Records, err = backend.SearchVisible(ctx, request)
			break
		}
		scopes := []Scope{{Type: ScopeGlobal, Value: "_global"}}
		if request.Workspace != "" {
			scopes = append(scopes, Scope{Type: ScopeWorkspace, Value: request.Workspace})
		}
		if request.Project != "" {
			scopes = append(scopes, Scope{Type: ScopeProject, Value: request.Project})
		}
		seen := make(map[int64]struct{})
		for i := len(scopes) - 1; i >= 0 && len(response.Records) < request.Limit; i-- {
			remaining := request.Limit - len(response.Records)
			var found []Record
			found, err = options.data.Search(ctx, scopes[i], request.Query, request.Kind, request.Tier, remaining)
			if err != nil {
				break
			}
			for _, record := range found {
				if _, exists := seen[record.ID]; exists {
					continue
				}
				seen[record.ID] = struct{}{}
				response.Records = append(response.Records, record)
			}
		}
	case "upsert-workflow":
		if options.placement != PlacementKB || request.Workspace == "" || request.SignalType == "" || request.Rule == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		workflowScope := Scope{Type: ScopeWorkspace, Value: request.Workspace}
		key := "workflow:" + request.Workspace + ":" + request.SignalType
		confidence := 1.0
		if request.Confidence != nil {
			confidence = *request.Confidence
		}
		var record Record
		record, err = options.data.Put(ctx, workflowScope, Record{Scope: workflowScope, Tier: "L1",
			Kind: "workflow", Key: key, Content: request.Rule, Confidence: confidence})
		response.Records = []Record{record}
	case "store":
		if request.Key == "" || request.Kind == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		confidence := 1.0
		if request.Confidence != nil {
			confidence = *request.Confidence
		}
		var record Record
		record, err = options.data.Put(ctx, scope, Record{Scope: scope, Tier: request.Tier,
			Kind: request.Kind, Key: request.Key, Content: request.Content, Confidence: confidence})
		response.Records = []Record{record}
	case "supersede":
		if request.ID <= 0 || request.Content == "" || request.Confidence == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		advanced, ok := options.data.(dataAdvancedStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var record Record
		record, err = advanced.Supersede(ctx, scope, request.ID, request.Content, *request.Confidence)
		if errors.Is(err, ErrMemoryNotFound) {
			err = nil
			response.Records = []Record{}
		} else {
			response.Records = []Record{record}
		}
	case "feedback":
		if len(request.IDs) == 0 || len(request.IDs) > 64 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		advanced, ok := options.data.(dataAdvancedStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		err = advanced.Feedback(ctx, scope, request.IDs, request.Success)
	case "maintenance":
		advanced, ok := options.data.(dataAdvancedStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Promoted, response.Demoted, response.Expired, err = advanced.Maintenance(ctx, scope)
	case "prospective-count":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		prospective, ok := options.data.(prospectiveStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Armed, response.Triggered, response.Completed, response.ProspectiveExpired, err =
			prospective.ProspectiveCounts(ctx)
	case "prospective-create", "prospective-list", "prospective-get", "prospective-complete",
		"prospective-sweep", "prospective-match", "prospective-mark-triggered":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		prospective, ok := options.data.(prospectiveDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "prospective-create":
			if request.TriggerText == "" || request.ActionText == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Prospective
			item, err = prospective.ProspectiveCreate(ctx, request)
			response.Prospectives = []Prospective{item}
		case "prospective-list":
			response.Prospectives, err = prospective.ProspectiveList(ctx, request.State, request.Limit)
		case "prospective-get":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Prospective
			item, err = prospective.ProspectiveGet(ctx, request.ID)
			response.Prospectives = []Prospective{item}
		case "prospective-complete":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = prospective.ProspectiveComplete(ctx, request.ID)
		case "prospective-sweep":
			response.ProspectiveExpired, err = prospective.ProspectiveSweep(ctx)
		case "prospective-match":
			response.Prospectives, err = prospective.ProspectiveMatch(ctx, request.Query,
				request.AnchorEntity, request.AnchorFile, request.Limit)
		case "prospective-mark-triggered":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = prospective.ProspectiveMarkTriggered(ctx, request.ID)
		}
	case "fact-recall":
		if options.placement != PlacementKB || request.ContentCapacity < 1 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		recall, ok := options.data.(factRecallStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var block string
		var count int
		block, count, err = recall.RecallFacts(ctx, request.Entity, request.Query,
			request.TurnRequestsSensitive, request.ContentCapacity)
		response.Block, response.Count = &block, &count
	case "directive-create", "directive-list", "directive-get", "directive-resolve", "directive-suppress",
		"directive-sweep", "directive-match", "directive-mark-surfaced", "directive-count":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		directives, ok := options.data.(directiveDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "directive-create":
			if request.Question == "" || request.Cause == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Directive
			item, err = directives.DirectiveCreate(ctx, request)
			response.Directives = []Directive{item}
		case "directive-list":
			response.Directives, err = directives.DirectiveList(ctx, request.State, request.Cause, request.Limit)
		case "directive-get":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Directive
			item, err = directives.DirectiveGet(ctx, request.ID)
			response.Directives = []Directive{item}
		case "directive-resolve":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = directives.DirectiveResolve(ctx, request.ID,
				request.ResolutionMemoryID, request.Note)
		case "directive-suppress":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = directives.DirectiveSuppress(ctx, request.ID)
		case "directive-sweep":
			response.Expired, err = directives.DirectiveSweep(ctx)
		case "directive-match":
			response.Directives, err = directives.DirectiveMatch(ctx, request.Query,
				request.AnchorEntity, request.AnchorFile, request.Limit)
		case "directive-mark-surfaced":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = directives.DirectiveMarkSurfaced(ctx, request.ID)
		case "directive-count":
			var counts DirectiveCounts
			counts, err = directives.DirectiveCounts(ctx)
			response.DirectiveCounts = &counts
		}
	case "touch", "update-content", "reject", "link-create", "link-query", "link-delete",
		"provenance-list", "provenance-add", "conflict-list", "conflict-record",
		"conflict-resolve", "scope-tag", "scope-collect", "scope-primary", "scope-rank", "health",
		"lifecycle-get", "lifecycle-transition", "lifecycle-pending", "lifecycle-sweep",
		"lifecycle-count", "episode-list", "episode-get", "relation-search", "entity-edges",
		"entity-profile", "fact-history":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		domain, ok := options.data.(domainDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "touch":
			ids := request.IDs
			if request.ID > 0 {
				ids = append(ids, request.ID)
			}
			if len(ids) == 0 || len(ids) > 256 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var count int
			count, err = domain.Touch(ctx, ids)
			response.Count = &count
		case "update-content":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.UpdateContent(ctx, request.ID, request.Content)
		case "reject":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.Reject(ctx, request.ID, request.Reason)
		case "link-create":
			if request.SourceID <= 0 || request.TargetID <= 0 || request.SourceID == request.TargetID || request.Relation == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item MemoryLink
			item, err = domain.LinkCreate(ctx, request.SourceID, request.TargetID, request.Relation)
			response.Links = []MemoryLink{item}
		case "link-query":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Links, err = domain.LinkQuery(ctx, request.ID, request.Limit)
		case "link-delete":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Deleted, err = domain.LinkDelete(ctx, request.ID)
		case "provenance-list":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Provenance, err = domain.ProvenanceList(ctx, request.ID, request.Limit)
		case "provenance-add":
			if request.ID <= 0 || request.ActionText == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Provenance
			item, err = domain.ProvenanceAdd(ctx, request.ID, request.SessionID, request.ActionText, request.Details)
			response.Provenance = []Provenance{item}
		case "conflict-list":
			response.Conflicts, err = domain.ConflictList(ctx, request.Limit)
		case "conflict-record":
			if request.SourceID <= 0 || request.TargetID <= 0 || request.SourceID == request.TargetID {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Conflict
			item, err = domain.ConflictRecord(ctx, request.SourceID, request.TargetID)
			response.Conflicts = []Conflict{item}
		case "conflict-resolve":
			if request.ID <= 0 || request.Resolution == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.ConflictResolve(ctx, request.ID, request.Resolution)
		case "scope-tag":
			if request.ID <= 0 || request.Scope.Type == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.ScopeTag(ctx, request.ID, scope)
		case "scope-collect":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Scopes, err = domain.ScopeCollect(ctx, request.ID)
		case "scope-primary":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var primary ScopeTag
			primary, err = domain.PrimaryScope(ctx, request.ID)
			response.Scopes = []ScopeTag{primary}
		case "scope-rank":
			ids := request.IDs
			if request.ID > 0 {
				ids = append(ids, request.ID)
			}
			if len(ids) == 0 || len(ids) > 256 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.ScopeRanks, err = domain.ScopeRanks(ctx, ids, request.Workspace, request.Project, request.IncludeAll)
		case "health":
			var health MemoryHealth
			health, err = domain.Health(ctx)
			response.Health = &health
		case "lifecycle-get":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.LifecycleState, err = domain.LifecycleGet(ctx, request.ID)
		case "lifecycle-transition":
			if request.ID <= 0 || request.LifecycleState == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.LifecycleTransition(ctx, request.ID, request.LifecycleState, request.ArchiveReason)
		case "lifecycle-pending":
			if request.ID <= 0 || request.TTLDays <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.LifecyclePending(ctx, request.ID, request.TTLDays)
		case "lifecycle-sweep":
			var count int
			count, err = domain.LifecycleSweep(ctx)
			response.Count = &count
		case "lifecycle-count":
			var counts LifecycleCounts
			counts, err = domain.LifecycleCounts(ctx)
			response.Lifecycle = &counts
		case "episode-list":
			response.Episodes, err = domain.EpisodeList(ctx, request.Query, request.Limit)
		case "episode-get":
			if request.Key == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var item Episode
			item, err = domain.EpisodeGet(ctx, request.Key)
			response.Episodes = []Episode{item}
		case "relation-search":
			response.Relations, err = domain.RelationSearch(ctx, request.Query, request.AsOf, request.Limit)
		case "entity-edges":
			if request.Entity == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Relations, err = domain.EntityEdges(ctx, request.Entity, request.Limit)
		case "entity-profile":
			if request.Entity == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var profile EntityProfile
			profile, err = domain.EntityProfile(ctx, request.Entity)
			response.EntityProfile = &profile
		case "fact-history":
			if request.Key == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Records, err = domain.FactHistory(ctx, request.Key, request.Limit)
		}
	case "valid-at":
		if options.placement != PlacementKB || request.ID <= 0 || request.AsOf == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		temporal, ok := options.data.(temporalStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var valid bool
		valid, err = temporal.ValidAt(ctx, request.ID, request.AsOf)
		response.ValidAt = &valid
	case "stats":
		domain, ok := options.data.(domainDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var stats MemoryStats
		stats, err = domain.Stats(ctx)
		response.Stats = &stats
	case "review-list":
		queries, ok := options.data.(queryDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Reviews, err = queries.ReviewList(ctx, request.State, request.Limit)
	case "key-exists", "find-id", "query-records", "low-effectiveness", "unused-l2",
		"superseded-keys", "restore", "set-artifact", "summaries", "scenes",
		"scene-members", "all-ids", "epistemic-kind", "demote-confidence", "tier-kind-counts":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		queries, ok := options.data.(queryDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "key-exists":
			if request.Key == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var exists bool
			exists, err = queries.KeyExists(ctx, request.Key)
			response.Allowed = &exists
		case "find-id":
			if request.Key == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var id int64
			id, err = queries.FindID(ctx, request.Key, request.Kind)
			response.IDs = []int64{id}
		case "query-records":
			response.Records, err = queries.QueryRecords(ctx, request.Mode, request.Pattern, request.Days, request.Limit)
		case "low-effectiveness":
			threshold := 0.5
			if request.Confidence != nil {
				threshold = *request.Confidence
			}
			response.LowEffectiveness, err = queries.LowEffectiveness(ctx, threshold, request.Limit)
		case "unused-l2":
			response.Records, err = queries.UnusedL2(ctx, request.Days, request.Limit)
		case "superseded-keys":
			response.SupersededKeys, err = queries.SupersededKeys(ctx, request.MinVersions, request.Limit)
		case "restore":
			if request.ID <= 0 || request.Actor == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = queries.Restore(ctx, request.ID, request.Actor)
		case "set-artifact":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = queries.SetArtifact(ctx, request.ID, request.ArtifactType, request.ArtifactRef, request.ArtifactHash)
		case "summaries":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Summaries, err = queries.Summaries(ctx, request.ID, request.Limit)
		case "scenes":
			response.Scenes, err = queries.Scenes(ctx, request.Limit)
		case "scene-members":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.SceneMembers, err = queries.SceneMembers(ctx, request.ID, request.Limit)
		case "all-ids":
			response.IDs, err = queries.AllIDs(ctx)
		case "epistemic-kind":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Name, err = queries.EpistemicKind(ctx, request.ID)
		case "demote-confidence":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = queries.DemoteConfidence(ctx, request.ID)
		case "tier-kind-counts":
			response.TierKindCounts, err = queries.TierKindCounts(ctx, request.Limit)
		}
	case "recall-bundle", "briefing-bundle", "alerts-bundle", "assemble-context", "context-block",
		"diagnose", "explain", "ask":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		retrieval, ok := options.data.(retrievalDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "recall-bundle":
			response.Payload, err = retrieval.RecallBundle(ctx, request.Query, request.LimitTokens, request.SessionStart)
		case "briefing-bundle":
			response.Payload, err = retrieval.BriefingBundle(ctx, request.LimitTokens)
		case "alerts-bundle":
			response.Payload, err = retrieval.AlertsBundle(ctx, request.AsOf)
		case "assemble-context", "context-block":
			var block string
			block, err = retrieval.AssembleContext(ctx, scope, request.Query, request.BlockType, request.Limit)
			response.Block = &block
		case "diagnose":
			response.Diagnostics, err = retrieval.Diagnose(ctx, scope, request.Query, request.Limit)
		case "explain":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var diagnostic Diagnostic
			diagnostic, err = retrieval.Explain(ctx, scope, request.Query, request.ID)
			response.Diagnostics = []Diagnostic{diagnostic}
		case "ask":
			var answer AnswerResult
			answer, err = retrieval.Ask(ctx, scope, request.Query, request.Limit)
			response.Answer = &answer
		}
	case "delete":
		if request.ID <= 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		response.Deleted, err = options.data.Delete(ctx, scope, request.ID)
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	if err != nil {
		if invocation.Cancelled() || ctx.Err() != nil {
			return nil, bus.ModuleStatusCancelled
		}
		return nil, bus.ModuleStatusInternal
	}
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if transaction != nil {
		if err := transaction.Commit(ctx); err != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	return encoded, bus.ModuleStatusOK
}
