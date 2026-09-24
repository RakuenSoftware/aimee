package memory

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"github.com/JBailes/aimee/server-go/modules/egress"
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
	HygienePreview      *hygienePreviewRequest `json:"hygiene_preview,omitempty"`
	AssemblyBudgetBytes json.RawMessage        `json:"assembly_budget_bytes,omitempty"`
	assemblyBytes       *int

	CorrectionReview *correctionReviewRequest `json:"correction_review,omitempty"`
	ProposalID       string                   `json:"proposal_id,omitempty"`
	IdempotencyKey   string                   `json:"idempotency_key,omitempty"`

	IncludeVersion  bool                 `json:"include_version,omitempty"`
	ExpectedVersion *MemoryRecordVersion `json:"expected_version,omitempty"`
	AtVersion       *MemoryRecordVersion `json:"at_version,omitempty"`

	CollectFactSources bool                `json:"collect_fact_sources,omitempty"`
	Revalidation       *sourceRevalidation `json:"revalidation,omitempty"`
	IngressPreview     bool                `json:"ingress_preview,omitempty"`

	Changes        *MemoryChangesRequest `json:"changes,omitempty"`
	ReadPolicy     *MemoryReadPolicy     `json:"read_policy,omitempty"`
	pageRankConfig *pageRankConfig
	requestedLimit int
	PageRank       *pageRankRequest        `json:"pagerank,omitempty"`
	lanes          recallLanes             // request-local attribution; never accepted from wire input
	TypedContext   *typedContextOptions    `json:"typed_context,omitempty"`
	Assertions     *assertionSearchRequest `json:"assertions,omitempty"`
	TraceBatch     *traceMiningBatch       `json:"trace_batch,omitempty"`
	Reflection     *reflectionOptions      `json:"reflection,omitempty"`
	Hops           int                     `json:"hops,omitempty"`
	Relations      []string                `json:"relations"`
	FactWork       *MemoryFactWork         `json:"fact_work,omitempty"`
	GraphPath      []GraphPathEntry        `json:"graph_path,omitempty"`
	CodePointIDs   []int64                 `json:"code_point_ids,omitempty"`
	AutomaticLimit bool                    `json:"automatic_limit,omitempty"`
	Detail         bool                    `json:"detail,omitempty"`
	Timings        bool                    `json:"timings,omitempty"`
	FailedOnly     bool                    `json:"failed_only,omitempty"`
	ResetStuck     bool                    `json:"reset_stuck,omitempty"`
	TagScope       *Scope                  `json:"tag_scope,omitempty"`
	PublicView     bool                    `json:"public_view,omitempty"`
	SharedRecall   json.RawMessage         `json:"shared_recall,omitempty"`
	Activation     json.RawMessage         `json:"activation,omitempty"`
	FactWrite      *FactWriteRequest       `json:"fact_write,omitempty"`
	CodeIndex      *CodeIndexRequest       `json:"code_index,omitempty"`
	Demotion       *DemotionConfig         `json:"demotion,omitempty"`
	// Accepted for old callers, but never used to override instance configuration.
	GraphCodeFusionState  string    `json:"graph_code_fusion_state,omitempty"`
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
	FactSource            string    `json:"fact_source,omitempty"`
	FactTarget            string    `json:"fact_target,omitempty"`
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
	Authorship *PersonalAuthorship  `json:"authorship,omitempty"`
	Version    *MemoryRecordVersion `json:"version,omitempty"`
	Historical bool                 `json:"historical,omitempty"`

	observedVersion *MemoryRecordVersion
	currentRead     bool
	historicalRead  bool
	rankingSteps    []rankingStep
	retrievalScore  float64
	retrievalBase   float64
	pageRankBonus   float64
	pageRankApplied bool
	graphScore      float64
	codeProximity   float64
	ID              int64   `json:"id"`
	Scope           Scope   `json:"scope"`
	Tier            string  `json:"tier"`
	Kind            string  `json:"kind"`
	Key             string  `json:"key"`
	Content         string  `json:"content"`
	Confidence      float64 `json:"confidence"`
}

type DataResponse struct {
	MemoryPreviews    []ingressMemoryPreview `json:"memory_previews,omitempty"`
	PreviewProjection *previewProjection     `json:"preview_projection,omitempty"`
	FactProjection    *factProjection        `json:"fact_projection,omitempty"`
	Proposal          *correctionProposal    `json:"proposal,omitempty"`
	MutationReceipt   *MemoryMutationReceipt `json:"mutation_receipt,omitempty"`

	Changes            *MemoryChangePage    `json:"changes,omitempty"`
	Read               *MemoryReadResult    `json:"read,omitempty"`
	ContextAssembly    *ContextAssembly     `json:"context_assembly,omitempty"`
	Dimension          int                  `json:"dimension,omitempty"`
	Embedding          *EmbedResponse       `json:"embedding,omitempty"`
	Version            string               `json:"version,omitempty"`
	PublicRecords      []publicMemoryRecord `json:"public_records,omitempty"`
	Deduplicated       bool                 `json:"deduplicated,omitempty"`
	FactWrite          *FactWriteDecision   `json:"fact_write,omitempty"`
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

	LegacyResults []LegacySearchResult `json:"legacy_results,omitempty"`
	VectorHits    []VectorHit          `json:"vector_hits,omitempty"`
	Drift         *DriftResult         `json:"drift,omitempty"`
	SummaryCount  int                  `json:"summary_count,omitempty"`
	FactCount     int                  `json:"fact_count,omitempty"`
	Failed        int                  `json:"failed,omitempty"`
	FactWork      *MemoryFactWork      `json:"fact_work,omitempty"`
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
	UpdateContent(context.Context, int64, string) (int64, error)
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
	ExportJSONL(context.Context, string, bool) (int, error)
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
	CompleteMemoryFact(context.Context, MemoryFactWork, string, bool, string) (bool, error)
}

type DataStore interface {
	Get(context.Context, Scope, int64) (Record, error)
	Search(context.Context, Scope, string, string, string, int) ([]Record, error)
	Put(context.Context, Scope, Record) (Record, error)
	Delete(context.Context, Scope, int64) (bool, error)
}

var ErrMemoryNotFound = errors.New("memory: record not found")

type postgresDataStore struct {
	personalActor   personalActor
	pageRankSamples *[]pageRankResult
	recallExecutor  egress.Executor
	requireSemantic bool // standalone evaluation must not silently fall back to lexical recall
	auditAction     func(context.Context, audit.Action) error
	auditBatch      *mutationAuditBatch
	episodeCommand  func(context.Context, string, []byte) ([]byte, error)
	settings        func() (map[string]any, error)
	fusionEnabled   bool
	code            codeIndexState
	personal        *personalVectors
	db              store.Queryer
	placement       Placement
}

func NewPostgresDataStore(db store.Queryer, placement Placement) (DataStore, error) {
	if db == nil {
		return nil, errors.New("memory: no postgres capability")
	}
	if placement != PlacementServer && placement != PlacementKB {
		return nil, fmt.Errorf("memory: invalid placement %q", placement)
	}
	enabled, err := instanceGraphFusion()
	if err != nil {
		return nil, err
	}
	backend := &postgresDataStore{db: db, placement: placement, fusionEnabled: enabled}
	if publisher, ok := db.(interface {
		MemoryAuditAction(context.Context, audit.Action) error
	}); ok {
		backend.auditAction = publisher.MemoryAuditAction
	}
	if configured, ok := db.(interface {
		MemorySettings() (map[string]any, error)
	}); ok {
		backend.settings = configured.MemorySettings
	}
	return backend, nil
}

func (s *postgresDataStore) Get(ctx context.Context, scope Scope, id int64) (Record, error) {
	return s.get(ctx, scope, id, false)
}

func (s *postgresDataStore) get(ctx context.Context, scope Scope, id int64, historical bool) (Record, error) {
	return s.getAt(ctx, scope, id, historical, "")
}

// A nonempty validAt is already normalized by the versioned read contract.
// Historical interval selection stays in the same query as scope/lifecycle
// admission, avoiding a second metadata round trip and a time-of-check gap.
func (s *postgresDataStore) getAt(ctx context.Context, scope Scope, id int64, historical bool, validAt string) (Record, error) {
	return s.getAtVersioned(ctx, scope, id, historical, validAt, false)
}
func (s *postgresDataStore) getAtVersioned(ctx context.Context, scope Scope, id int64, historical bool, validAt string, includeVersion bool) (Record, error) {
	var r Record
	if s.placement == PlacementServer {
		r.Scope = scope
		columns := "id,tier,kind,key,content,confidence"
		destinations := []any{&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence}
		if includeVersion {
			r.Version = &MemoryRecordVersion{SchemaVersion: 1, RecordID: strconv.FormatInt(id, 10)}
			columns += ",(SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1),record_revision::text"
			destinations = append(destinations, &r.Version.OwnerID, &r.Version.RecordRevision)
			r.Authorship = &PersonalAuthorship{}
			columns += ",provenance_category,author_principal,author_transport,reviewer_principal,reviewer_transport,review_proposal_id"
			destinations = append(destinations, &r.Authorship.Category, &r.Authorship.Principal, &r.Authorship.Transport, &r.Authorship.Reviewer, &r.Authorship.ReviewTransport, &r.Authorship.ProposalID)
		}
		err := s.db.QueryRow(ctx, `SELECT `+columns+`
FROM user_memories
WHERE id = $1 AND `+personalCurrentMemorySQL(""), id).
			Scan(destinations...)
		if store.IsNoRows(err) {
			return Record{}, ErrMemoryNotFound
		}
		return r, err
	}
	predicate := currentMemorySQL("")
	if historical {
		predicate = historicalMemoryInspectionSQL("")
	}
	parameters := []any{id}
	if validAt != "" {
		predicate += " AND " + memoryValidityAtSQL("", "$2::timestamptz")
		parameters = append(parameters, validAt)
	}
	columns := "id, scope_type, scope_value, tier, kind, key, content, confidence"
	destinations := []any{&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence}
	r.observedVersion = &MemoryRecordVersion{SchemaVersion: 1, RecordID: strconv.FormatInt(id, 10)}
	r.currentRead, r.historicalRead = !historical, historical
	columns += s.recallVersionColumns()
	destinations = append(destinations, &r.observedVersion.OwnerID, &r.observedVersion.RecordRevision)
	if includeVersion {
		r.Version = r.observedVersion
	}
	err := s.db.QueryRow(ctx, "SELECT "+columns+" FROM memories WHERE id=$1 AND "+predicate, parameters...).Scan(destinations...)
	if store.IsNoRows(err) {
		return Record{}, ErrMemoryNotFound
	}
	return r, err
}

func (s *postgresDataStore) upsertEmbedding(ctx context.Context, record Record, vector []float32) error {
	if s.placement == PlacementServer {
		return errors.New("personal vectors require the owner-selected serving identity")
	}
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
		err = s.retainActiveEmbedding(ctx, record.ID, vector)
	}
	if err == nil {
		_, err = s.db.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,indexed_at,updated_at)
VALUES($1,'memory',$1,'ok',0,'',pg_now_text(),pg_now_text()) ON CONFLICT(point_id) DO UPDATE SET
status='ok',last_error='',indexed_at=pg_now_text(),updated_at=pg_now_text()`, record.ID)
	}
	return err
}

func (s *postgresDataStore) Supersede(ctx context.Context, scope Scope, id int64, content string, confidence float64) (out Record, err error) {
	defer func() {
		if s.placement == PlacementServer {
			s.recordMutation(DataRequest{Operation: "supersede", ID: id}, DataResponse{Records: []Record{out}}, err, "")
		}
	}()
	var screenErr error
	content, screenErr = screenMemoryText(content)
	if screenErr != nil {
		return Record{}, screenErr
	}
	if s.placement == PlacementServer {
		return s.mutatePersonal(ctx, "supersede", Record{Scope: scope, ID: id, Content: content, Confidence: confidence}, nil)
	}
	return s.supersedeKB(ctx, id, content, confidence, "")
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
		_, err := s.db.Exec(ctx, `WITH cited AS MATERIALIZED (
 SELECT id,key FROM memories WHERE id=$2 AND lifecycle_state='active' AND activation_suppressed=0
), edges AS (
 UPDATE entity_edges SET utility_score=GREATEST(-5.0,LEAST(5.0,COALESCE(utility_score,0)+$1)),utility_touched_at=pg_now_text()
 WHERE edge_class<>'semantic' AND (source IN (SELECT key FROM cited) OR target IN (SELECT key FROM cited)) RETURNING 1
) INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
 SELECT id,key,'corrected_by',key,'Feedback correction' FROM cited WHERE $1<0`, delta, id)
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
		if db, ok := s.db.(store.DB); ok {
			tx, err := db.Begin(ctx)
			if err != nil {
				return 0, 0, 0, err
			}
			defer tx.Rollback(context.WithoutCancel(ctx))
			bound := *s
			bound.db = tx
			promoted, demoted, expired, err := bound.Maintenance(ctx, scope)
			if err != nil {
				return 0, 0, 0, err
			}
			if err := tx.Commit(ctx); err != nil {
				return 0, 0, 0, err
			}
			return promoted, demoted, expired, nil
		}
		if _, ok := s.db.(store.Tx); !ok {
			return 0, 0, 0, errors.New("memory: private maintenance requires a transaction")
		}
		if _, err := s.db.Exec(ctx, `SELECT set_config('aimee.private_authority','model',true),set_config('aimee.private_principal','system:memory-maintenance',true),set_config('aimee.private_transport','internal',true)`); err != nil {
			return 0, 0, 0, err
		}
		// Background hygiene cannot rewrite user/unknown authorship or protected
		// kinds. Those candidates require the proposal/review workflow.
		table, where, args = "user_memories", "provenance_category='agent_message' AND kind NOT IN ('episode','experience','instruction','policy') AND lifecycle_state='active' AND ", nil
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
	req, planErr := s.planRecall(DataRequest{Scope: scope, Query: query, Kind: kind, Tier: tier, Limit: limit})
	if planErr != nil {
		return nil, planErr
	}
	limit = req.Limit
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
  updated_at DESC, id DESC LIMIT $4`, pattern, kind, tier, limit, query)
	} else {
		rows, err = s.db.Query(ctx, `SELECT `+queryRecordColumns+`
FROM memories
WHERE `+currentMemorySQL("")+` AND scope_type = $1 AND scope_value = $2
  AND ($7 = '' OR key ILIKE $3 OR content ILIKE $3 OR use_cases ILIKE $3
       OR to_tsvector('english', key || ' ' || content || ' ' || COALESCE(use_cases,''))
          @@ plainto_tsquery('english', $7))
  AND ($4 = '' OR kind = $4) AND ($5 = '' OR tier = $5)
ORDER BY (lower(key)=lower($7)) DESC,
  ts_rank_cd(to_tsvector('english', key || ' ' || content || ' ' || COALESCE(use_cases,'')),
             plainto_tsquery('english', $7)) DESC,
  updated_at DESC, id DESC LIMIT $6`,
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
			r.observedVersion = &MemoryRecordVersion{SchemaVersion: 1}
			r.currentRead = true
			err = rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind,
				&r.Key, &r.Content, &r.Confidence, &r.observedVersion.OwnerID, &r.observedVersion.RecordRevision)
			r.observedVersion.RecordID = strconv.FormatInt(r.ID, 10)
		}
		if err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows.Close()
	lanes := recallLanes{}
	lanes.add(records, laneLexical)
	if s.personal != nil && query != "" {
		// Leave time to return the local lexical result when DNS or the model
		// stalls. Consuming the bus deadline would discard that valid result.
		budget := 1500 * time.Millisecond
		if deadline, ok := ctx.Deadline(); ok && time.Until(deadline)/2 < budget {
			budget = time.Until(deadline) / 2
		}
		semanticCtx, cancel := context.WithTimeout(ctx, budget)
		semantic, err := s.personal.search(semanticCtx, query, kind, tier, limit)
		cancel()
		if err == nil {
			lanes.add(semantic, laneSemantic)
			records = fuseRanked(ctx, records, semantic, limit, "lexical", "semantic")
		}
	}
	req.lanes = lanes
	return s.finalizeRecall(ctx, req, true, records)
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

func (s *postgresDataStore) Put(ctx context.Context, scope Scope, r Record) (out Record, err error) {
	defer func() {
		if s.placement == PlacementServer {
			s.recordMutation(DataRequest{Operation: "store"}, DataResponse{Records: []Record{out}}, err, "")
		}
	}()
	var screenErr error
	r.Content, screenErr = screenMemoryWrite(r.Key, r.Content)
	if screenErr != nil {
		return Record{}, screenErr
	}
	if r.Tier == "" {
		r.Tier = "L2"
	}
	r.Scope = scope
	if s.placement == PlacementServer {
		return s.mutatePersonal(ctx, "store", r, nil)
	}
	return s.InsertEpistemic(ctx, DataRequest{Scope: scope, Tier: r.Tier, Kind: r.Kind, Key: r.Key, Content: r.Content, Confidence: &r.Confidence, Authority: AuthorityModel})
}

func (s *postgresDataStore) Delete(ctx context.Context, scope Scope, id int64) (changed bool, err error) {
	if s.placement == PlacementKB {
		// Mutation admission is distinct from serving eligibility. An expired or
		// suppressed active record can still be retired by its authorized author.
		// Read only its identity here; DeleteAs owns the authority decision.
		var currentScope Scope
		err := s.db.QueryRow(ctx, `SELECT scope_type,scope_value FROM memories
 WHERE id=$1 AND lifecycle_state='active'`, id).Scan(&currentScope.Type, &currentScope.Value)
		if store.IsNoRows(err) {
			return false, nil
		}
		if err != nil {
			return false, err
		}
		if currentScope != scope {
			return false, nil
		}
		return s.DeleteAs(ctx, id, AuthorityModel)
	}

	defer func() {
		s.recordMutation(DataRequest{Operation: "delete", ID: id}, DataResponse{Deleted: changed}, err, "memory.retire")
	}()
	_, err = s.mutatePersonal(ctx, "delete", Record{Scope: scope, ID: id}, nil)
	if errors.Is(err, ErrMemoryNotFound) {
		return false, nil
	}
	return err == nil, err
}

type handlerOptions struct {
	dataContext    context.Context
	gateway        *gatewayState
	executor       egress.Executor
	placement      Placement
	data           DataStore
	commandContext *bus.CommandContext
	publicWrite    bool
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
	if len(request.AssemblyBudgetBytes) != 0 {
		if request.Operation != "assemble-context" {
			return DataRequest{}, errors.New("memory: assembly budget requires assemble-context")
		}
		var err error
		request.assemblyBytes, err = commandByteLimit(commandArgs{"budget_bytes": request.AssemblyBudgetBytes}, "budget_bytes")
		if err != nil {
			return DataRequest{}, err
		}
	}
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
	maxLimit := 100
	switch request.Operation {
	case "ontology-walk":
		maxLimit = 128
	case "scene-members":
		maxLimit = 512
	case "vector-repair-prepare", "vector-embed-prepare":
		maxLimit = 1024
	case "rebuild-derived":
		maxLimit = 100000
	case "prospective-list", "directive-list", "prospective-current", "directive-current", "lint", "conflict-list", "low-effectiveness", "unused-l2", "superseded-keys", "entity-edges":
		maxLimit = 256
	}
	if request.Limit < 1 || request.Limit > maxLimit || len(request.Kind) > 64 ||
		len(request.Tier) > 16 || len(request.Key) > 4096 || len(request.Query) > 16384 ||
		len(request.Content) > 512*1024 || len(request.Workspace) > 1024 ||
		len(request.Project) > 1024 || len(request.SignalType) > 64 || len(request.Rule) > 512*1024 ||
		len(request.SessionID) > 256 || len(request.Client) > 64 || len(request.Tool) > 64 ||
		len(request.Path) > 4096 || len(request.Home) > 4096 || len(request.Command) > 65536 ||
		len(request.ProjectsRoot) > 4096 || len(request.MemorySegment) > 256 ||
		len(request.FactSource) > 1024 || len(request.FactTarget) > 1024 ||
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
		len(request.ArtifactHash) > 256 || len(request.Actor) > 576 ||
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

func handleData(options handlerOptions, invocation bus.ModuleInvocation, body []byte) (result []byte, status bus.ModuleStatus) {
	if backend, ok := options.data.(*postgresDataStore); ok {
		bound := *backend
		bound.recallExecutor = options.executor
		bound.pageRankSamples = &[]pageRankResult{}
		defer func() {
			if status == bus.ModuleStatusOK {
				for _, sample := range *bound.pageRankSamples {
					pageRankMetricState.observe(sample)
				}
			}
		}()
		options.data = &bound
	}
	request, err := decodeDataRequest(body)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if request.HygienePreview != nil && request.Operation != "hygiene-preview" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if request.Changes != nil && request.Operation != "change-feed" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if request.IncludeVersion && request.Operation != "get" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if request.IngressPreview && (!request.PublicView || (request.Operation != "diagnose" && request.Operation != "explain")) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if request.AtVersion != nil && (options.placement != PlacementServer || request.Operation != "get" || !request.AtVersion.validFor(request.ID) || request.ReadPolicy != nil || request.AsOf != "") {
		return nil, bus.ModuleStatusInvalidRequest
	}
	versionedMutation := (options.placement == PlacementKB && (versionedCorrectionOperation(request.Operation) || request.Operation == "delete-as" || request.Operation == "reject" || request.Operation == "restore")) || (options.placement == PlacementServer && (request.Operation == "supersede" || request.Operation == "delete"))
	if request.ExpectedVersion != nil && (!versionedMutation || !request.ExpectedVersion.validFor(request.ID)) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	creation := (options.placement == PlacementServer && request.Operation == "store") || (options.placement == PlacementKB && request.Operation == "insert-epistemic")
	if request.IdempotencyKey != "" && ((!creation && (!versionedMutation || request.ExpectedVersion == nil)) || !validIdempotencyKey(request.IdempotencyKey) || !verifiedRetryCaller(options.commandContext)) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementServer {
		bound := *backend
		bound.personalActor = personalCaller(options.commandContext, request.Authority)
		options.data = &bound
	}
	var readResult *MemoryReadResult
	if request.ReadPolicy != nil {
		operation := request.Operation
		if operation == "validity" {
			operation = "get"
		}
		readResult = request.ReadPolicy.validate(options.placement, operation, request.AsOf)
		if readResult.ErrorCode != "" {
			encoded, encodeErr := json.Marshal(DataResponse{Read: readResult, Records: []Record{}})
			if encodeErr != nil {
				return nil, bus.ModuleStatusInternal
			}
			return encoded, bus.ModuleStatusOK
		}
	}
	// Preserve the caller's query shape: an inherited credential restriction is
	// an audience bound, not an explicit exact-scope request. Ordinary audience
	// reads retain shared/global rows while RLS excludes foreign projects.
	explicitScope := request.Scope.Type != "" || request.Scope.Value != ""
	if caller := options.commandContext; options.placement == PlacementKB && caller != nil {
		if err := bindVerifiedScope(&request, caller.ScopeKind, caller.ScopeID); err != nil {
			if request.Operation == "validity" {
				payload, _ := json.Marshal(commandError("unauthorized", "diagnostic scope exceeds authenticated scope"))
				encoded, _ := json.Marshal(DataResponse{Payload: payload})
				return encoded, bus.ModuleStatusOK
			}
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
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
	if request.Operation == "fact-write-decision" {
		if request.FactWrite == nil || len(request.FactWrite.Relation) > relTypeMax {
			return nil, bus.ModuleStatusInvalidRequest
		}
		decision := DecideFactWrite(*request.FactWrite)
		encoded, err := json.Marshal(DataResponse{FactWrite: &decision})
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
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
	if request.Operation == "fusion-state-set" || request.Operation == "fusion-state-clear" || request.Operation == "fusion-state-get" {
		// Legacy set/clear calls are read-only. They cannot mutate instance policy.
		var enabled bool
		var err error
		if configured, ok := options.data.(*postgresDataStore); ok {
			enabled = configured.graphFusionEnabled()
		} else {
			enabled, err = instanceGraphFusion()
		}
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		encoded, err := json.Marshal(DataResponse{Allowed: &enabled})
		if err != nil {
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
	budget := dataTimeout
	if request.Operation == "demotion-run" || request.Operation == "demotion-check" {
		budget = 120 * time.Second
	}
	if request.Operation == "cognify" || request.Operation == "cognify-drain" || request.Operation == "reflect" || request.Operation == "typed-context" {
		budget = 60 * time.Second
	}
	if request.Operation == "vector-verify" || request.Operation == "assertion-search" {
		budget = 30 * time.Second
	}
	if request.Operation == "vector-repair-record" || request.Operation == "episode-card-generate" {
		budget = embedHTTPTimeout()
	}
	timeout := invocation.Remaining(budget)
	if timeout <= 0 {
		return nil, bus.ModuleStatusCancelled
	}
	parent := options.dataContext
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()
	defer func() {
		if status != bus.ModuleStatusInternal && status != bus.ModuleStatusCancelled {
			return
		}
		cause := err
		if cause == nil {
			cause = ctx.Err()
		}
		// Never log the request, SQL, driver message or connection string.
		log.Printf("memory data failure operation=%q trace=%d status=%d class=%s",
			memoryFailureOperation(request.Operation), invocation.TraceID, status, memoryFailureClass(cause))
	}()
	if request.Operation == "code-index" {
		code, ok := options.data.(*postgresDataStore)
		if !ok || request.CodeIndex == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		payload, err := code.CodeIndex(ctx, *request.CodeIndex)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		encoded, err := json.Marshal(DataResponse{Payload: payload})
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}

	response := DataResponse{Read: readResult}
	rollbackOnly := false
	// Pin policy to one lazy snapshot per request, including its error. A
	// request must not mix settings from successive configuration generations;
	// the next request still observes changes immediately.
	if backend, ok := options.data.(*postgresDataStore); ok && backend.settings != nil {
		bound := *backend
		bound.settings = sync.OnceValues(backend.settings)
		options.data = &bound
	}
	if backend, ok := options.data.(*postgresDataStore); ok && backend.auditAction != nil {
		bound := *backend
		bound.auditBatch = &mutationAuditBatch{}
		options.data = &bound
		defer func() {
			if rollbackOnly {
				publishMutationAudit(bound.auditAction, request, response, status)
				return
			}
			if status == bus.ModuleStatusOK && len(bound.auditBatch.actions) > 0 {
				bound.auditBatch.flush(bound.auditAction)
			} else {
				publishMutationAudit(bound.auditAction, request, response, status)
			}
		}()
	}

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
			transaction = backend.auditTransaction(transaction)
			defer transaction.Rollback(context.Background())
			principal, authority, transport := "system:model-inference", "model", "internal"
			if caller := options.commandContext; caller != nil && caller.Authenticated {
				principal, transport = caller.Principal, caller.TransportIdentity
				if transport == "" {
					transport = principal
				}
				// The initiator and the content's authority are separate. Merely
				// authenticating a model request never upgrades its content.
				if (request.Authority == AuthorityUser && caller.UserAuthority) || request.Operation == "restore" {
					authority = "user"
				}
			}
			// These are bounded request queries, including nested source fences.
			// Compiling their expressions with PostgreSQL JIT can exceed the
			// whole request latency budget before any rows are read. Keep this
			// setting transaction-local; pooled connections retain their default.
			_, err = transaction.Exec(ctx, `SELECT
set_config('jit','off',true),
set_config('aimee.memory_scope_type',$1,true),
set_config('aimee.memory_scope_value',$2,true),
set_config('aimee.memory_workspace',$3,true),
set_config('aimee.memory_project',$4,true),
set_config('aimee.memory_scope_all',$5,true),
set_config('aimee.principal',$6,true),
set_config('aimee.authority',$7,true),
set_config('aimee.transport_identity',$8,true),
set_config('aimee.correlation_id',$9,true)`,
				string(scope.Type), scope.Value, request.Workspace, request.Project,
				map[bool]string{false: "0", true: "1"}[request.IncludeAll],
				principal, authority, transport, strconv.FormatUint(invocation.TraceID, 10))
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			bound := *backend
			bound.db = transaction
			options.data = &bound
		}
	}

	switch request.Operation {
	case "validity":
		caller := options.commandContext
		if caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" || invocation.PrincipalRef != 0 {
			response.Payload, _ = json.Marshal(commandError("unauthorized", "validity diagnostics require an authenticated user purpose"))
			break
		}
		if request.ID <= 0 || readResult == nil || request.IncludeAll {
			return nil, bus.ModuleStatusInvalidRequest
		}
		// The verified service identity spans the deployment data plane, as
		// kb_scope_authorized specifies. It still needs authenticated user
		// purpose here, and never turns into a memory scope or include_all.
		if options.placement == PlacementKB && caller.ScopeKind != "" && (caller.ScopeKind != "service" || caller.ScopeID == "") {
			authorized, scopeErr := normalizeScope(PlacementKB, Scope{Type: caller.ScopeKind, Value: caller.ScopeID})
			if scopeErr != nil || scope != authorized ||
				(request.Project != "" && (authorized.Type != ScopeProject || request.Project != authorized.Value)) ||
				(request.Workspace != "" && (authorized.Type != ScopeWorkspace || request.Workspace != authorized.Value)) {
				response.Payload, _ = json.Marshal(commandError("unauthorized", "diagnostic scope exceeds authenticated scope"))
				break
			}
		}
		backend, ok := options.data.(*postgresDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var decision EligibilityDecision
		decision, err = backend.validity(ctx, request.ID, readResult)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "store": map[Placement]string{PlacementServer: "user", PlacementKB: "kb"}[options.placement], "decision": decision})
		}
	case "css-convention-sync", "css-conventions":
		backend, ok := options.data.(*postgresDataStore)
		if invocation.PrincipalRef != 0 || options.placement != PlacementKB || !ok || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Project == "" || len(request.Project) > 4096 || scope.Type != ScopeProject || scope.Value != request.Project {
			return nil, bus.ModuleStatusInvalidRequest
		}
		result := map[string]any{"status": "ok", "project": request.Project}
		if request.Operation == "css-convention-sync" {
			result["op"] = "assert-conventions"
			result["asserted"], err = backend.syncCSSConventions(ctx, request.Project)
		} else {
			result["op"] = "conventions"
			var items []cssConvention
			items, err = backend.cssConventions(ctx, request.Project)
			result["results"], result["count"] = items, len(items)
		}
		if err == nil {
			response.Payload, err = json.Marshal(result)
			if len(response.Payload) > maxDataBody {
				err = errors.New("memory: CSS conventions exceed response capacity")
			}
		}
	case "hygiene-preview":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || transaction == nil || !explicitScope || request.IncludeAll || !request.HygienePreview.valid() {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var preview hygienePreview
		preview, err = backend.previewHygiene(ctx, scope, request.HygienePreview)
		if err == nil {
			response.Payload, err = json.Marshal(preview)
		}
	case "personal-source-revalidate":
		backend, ok := options.data.(*postgresDataStore)
		if invocation.PrincipalRef != 0 || options.placement != PlacementServer || !ok || !request.Revalidation.valid() {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var eligible bool
		eligible, err = backend.revalidatePersonalSources(ctx, request.Revalidation)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "eligible": eligible, "check_id": request.Revalidation.CheckID, "sources_digest": releaseDigest(request.Revalidation.Sources)})
		}
	case "source-revalidate":
		backend, ok := options.data.(*postgresDataStore)
		if invocation.PrincipalRef != 0 || options.placement != PlacementKB || !ok || transaction == nil || !request.Revalidation.valid() {
			return nil, bus.ModuleStatusInvalidRequest
		}
		exact := Scope{}
		if explicitScope {
			exact = scope
		}
		var eligible bool
		eligible, err = backend.revalidateSources(ctx, request.Revalidation, exact)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "eligible": eligible,
				"check_id": request.Revalidation.CheckID, "sources_digest": releaseDigest(request.Revalidation.Sources)})
		}
	case "typed-context":
		backend, ok := options.data.(*postgresDataStore)
		if invocation.PrincipalRef != 0 || options.placement != PlacementKB || !ok || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.TypedContext == nil || request.Assertions == nil || request.Query == "" || request.Limit != 32 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if p := request.TypedContext.Requirements; p != nil && !p.valid() {
			return nil, bus.ModuleStatusInvalidRequest
		}
		for name := range typedBudgetDefaults {
			n, ok := request.TypedContext.Budgets[name]
			if !ok || n < 0 || n > 4096 {
				return nil, bus.ModuleStatusInvalidRequest
			}
		}
		if _, budgetErr := request.TypedContext.ContextLimits.byteLimit(maxDataBody); budgetErr != nil {
			var refusal *contextBudgetError
			if !errors.As(budgetErr, &refusal) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Payload, err = json.Marshal(commandError(refusal.kind, refusal.message))
			break
		}
		if explicitScope {
			request.Scope = scope
		}
		var result typedContextResult
		result, err = backend.assembleTypedContext(ctx, invocation.TraceID, options.executor, request, explicitScope)
		if err == nil {
			response.Payload, err = json.Marshal(result)
			if len(response.Payload) > maxDataBody {
				err = errors.New("memory: typed context exceeds response capacity")
			}
		}
	case "assertion-search":
		backend, ok := options.data.(*postgresDataStore)
		if invocation.PrincipalRef != 0 || options.placement != PlacementKB || !ok || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Assertions == nil || request.Query == "" || request.Limit < 1 || request.Limit > 64 || request.Assertions.Hops < 0 || request.Assertions.Hops > 2 || !assertionTimestamp(request.Assertions.ValidAt) || !assertionTimestamp(request.Assertions.BelievedAt) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if explicitScope {
			request.Scope = scope
		}
		var result map[string]any
		result, err = backend.searchAssertions(ctx, invocation.TraceID, options.executor, request, explicitScope)
		if err == nil {
			response.Payload, err = json.Marshal(result)
			if len(response.Payload) > maxDataBody {
				err = errors.New("memory: assertion response exceeds capacity")
			}
		}
	case "pagerank":
		if invocation.PrincipalRef != 0 || !validPageRankRequest(request.PageRank) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var ranked pageRankResult
		ranked, err = backend.pageRank(ctx, request, explicitScope)
		if err == nil {
			response.Payload, err = json.Marshal(ranked)
			backend.recordPageRankSample(ranked)
		}
	case "trace-state", "trace-apply":
		backend, ok := options.data.(*postgresDataStore)
		if invocation.PrincipalRef != 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if !ok || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var result map[string]any
		if request.Operation == "trace-state" {
			var id int64
			id, err = backend.traceCursor(ctx)
			result = map[string]any{"status": "ok", "last_id": strconv.FormatInt(id, 10)}
		} else {
			if !validTraceBatch(request.TraceBatch) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			request.Scope = scope
			result, err = backend.applyTraceBatch(ctx, request)
		}
		if err == nil {
			response.Payload, err = json.Marshal(result)
		}
	case "wiki-bundle":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Payload, err = backend.wiki(ctx, request)
	case "reflect":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Reflection == nil || request.Query == "" || len(request.Query) > 2047 || request.Limit < 1 || request.Limit > 32 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var result reflectionResult
		result, err = backend.reflectMemories(ctx, request, explicitScope)
		if err == nil {
			response.Payload, err = json.Marshal(result)
		}
	case "hybrid-context":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || invocation.PrincipalRef != 0 || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var result hybridMemoryResult
		result, err = backend.hybridContext(ctx, request)
		if err == nil {
			response.Payload, err = json.Marshal(result)
		}
	case "convention-extract":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || invocation.PrincipalRef != 0 || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var count int
		count, err = backend.extractConventions(ctx, request)
		response.Count = &count
	case "ontology-walk":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Entity == "" || len(request.Entity) > 1024 || request.Hops < 0 || request.Hops > 128 || len(request.Relations) > 32 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		for _, name := range request.Relations {
			if name == "" || len(name) > 128 {
				return nil, bus.ModuleStatusInvalidRequest
			}
		}
		var entries []ontologyWalkEntry
		entries, err = backend.ontologyWalk(ctx, request, explicitScope)
		if err == nil {
			response.Payload, err = json.Marshal(entries)
		}
	case "demotion-run", "demotion-check":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB || transaction == nil || request.Demotion == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var summary any
		if request.Operation == "demotion-check" {
			summary, err = backend.previewDemotion(ctx, *request.Demotion)
		} else {
			summary, err = backend.runDemotion(ctx, *request.Demotion)
		}
		if err == nil {
			response.Payload, err = json.Marshal(summary)
		}

	case "fact-retract":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		actor := modelFactActor()
		if caller := options.commandContext; request.Authority == AuthorityUser && caller != nil && caller.Authenticated && caller.UserAuthority && caller.Principal != "" {
			actor = FactActor{Principal: caller.Principal, TransportIdentity: caller.TransportIdentity, Role: "user", Rank: 30, Authenticated: 1}
			if actor.TransportIdentity == "" {
				actor.TransportIdentity = actor.Principal
			}
		}
		var count int
		count, err = backend.invalidateFacts(ctx, actor, request.FactSource, request.Relation, request.FactTarget)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "retracted": count, "authority": actor.Role})
		}
	case "entity-conflicts":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var result map[string]any
		result, err = backend.entityConflicts(ctx, request)
		if err == nil {
			response.Payload, err = json.Marshal(result)
		}
	case "entity-review", "entity-mutate":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		actor := FactActor{Principal: "system:kb-maintenance", TransportIdentity: "internal", Role: "system", Rank: 20}
		if request.Operation == "entity-review" {
			caller := options.commandContext
			if caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			actor = FactActor{Principal: caller.Principal, TransportIdentity: caller.TransportIdentity, Role: "operator", Rank: 40, Authenticated: 1}
			if actor.TransportIdentity == "" {
				actor.TransportIdentity = actor.Principal
			}
		}
		var result map[string]any
		result, err = backend.mutateEntity(ctx, actor, request.State, request.SourceID, request.TargetID, request.ID)
		if err == nil {
			response.Updated = true
			response.Payload, err = json.Marshal(result)
		}
	case "ontology-dashboard", "ontology-review":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var result map[string]any
		if request.Operation == "ontology-review" {
			caller := options.commandContext
			if caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" {
				return nil, bus.ModuleStatusInvalidRequest
			}
			actor := FactActor{Principal: caller.Principal, TransportIdentity: caller.TransportIdentity, Role: "operator", Rank: 40, Authenticated: 1}
			if actor.TransportIdentity == "" {
				actor.TransportIdentity = actor.Principal
			}
			result, err = backend.reviewOntology(ctx, actor, request.State, request.Relation, request.FactTarget)
		} else {
			result, err = backend.ontologyDashboard(ctx)
		}
		if err == nil {
			response.Payload, err = json.Marshal(result)
			if len(response.Payload) > maxDataBody {
				err = errors.New("memory: ontology result exceeds capacity")
			}
		}
	case "fact-maintenance":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var count int
		count, err = backend.maintainFacts(ctx, request.State, request.Days)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "changed": count})
		}
	case "fact-candidates":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var candidates []map[string]any
		candidates, err = backend.factCandidates(ctx, request.Limit)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "candidates": candidates})
		}
	case "correction-proposals", "correction-review":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || (options.placement == PlacementKB && transaction == nil) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var result any
		if request.Operation == "correction-proposals" {
			if request.ProposalID != "" && !validProposalID(request.ProposalID) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var proposals []correctionProposal
			if options.placement == PlacementServer {
				proposals, err = backend.listPersonalCorrectionProposals(ctx, request.ProposalID, request.Limit)
			} else {
				proposals, err = backend.listCorrectionProposals(ctx, request.ProposalID, request.Limit)
			}
			result = map[string]any{"status": "ok", "proposals": proposals}
		} else {
			caller := options.commandContext
			if !verifiedRetryCaller(caller) || !caller.UserAuthority || !request.CorrectionReview.valid() {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var proposal correctionProposal
			if options.placement == PlacementServer {
				proposal, err = backend.reviewPersonalCorrection(ctx, *request.CorrectionReview, caller)
			} else {
				proposal, err = backend.reviewKBCorrection(ctx, *request.CorrectionReview, caller)
			}
			rollbackOnly = err != nil
			result = map[string]any{"status": "ok", "proposal": proposal}
			if errors.Is(err, ErrMemoryNotFound) || errors.Is(err, errCorrectionReviewConflict) {
				kind := "conflict"
				if errors.Is(err, ErrMemoryNotFound) {
					kind = "not_found"
				}
				result = commandError(kind, "correction review refused: hidden, missing, mismatched or already decided")
				err = nil
			}
		}
		if err == nil {
			if options.placement == PlacementServer {
				if envelope, ok := result.(map[string]any); ok && envelope["status"] == "ok" {
					envelope["store"] = "user"
				}
			}
			response.Payload, err = json.Marshal(result)
		}
	case "fact-review":
		backend, ok := options.data.(*postgresDataStore)
		caller := options.commandContext
		if !ok || invocation.PrincipalRef != 0 || caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		actor := FactActor{Principal: caller.Principal, TransportIdentity: caller.TransportIdentity, Role: "operator", Rank: 40, Authenticated: 1}
		if actor.TransportIdentity == "" {
			actor.TransportIdentity = actor.Principal
		}
		var result factMutationResult
		result, err = backend.reviewFact(ctx, actor, request.ID, request.State)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"status": "ok", "ok": true, "assertion_id": result.AssertionID, "lifecycle": result.Lifecycle, "commit_id": result.CommitID})
		}
	case "cognify", "cognify-drain", "cognify-status":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Payload, err = backend.cognifyData(ctx, request)
	case "reembed-prepare", "reembed-next", "reembed-point", "reembed-status", "reembed-cutover":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Payload, err = backend.reembedData(ctx, invocation.TraceID, options.executor, request)
	case "episode-cards":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.SessionID == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var cards []string
		cards, err = backend.episodeCards(ctx, request.SessionID, request.Limit)
		if err == nil {
			response.Payload, err = json.Marshal(map[string]any{"cards": cards})
		}

	case "vector-verify":
		backend, ok := options.data.(*postgresDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Payload, err = backend.verifyVectors(ctx, invocation.TraceID, options.executor, request)

	case "vector-repair-prepare", "vector-embed-prepare":
		backend, ok := options.data.(*postgresDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Operation == "vector-embed-prepare" {
			response, err = backend.prepareVectorEmbed(ctx, request)
		} else {
			response, err = backend.prepareVectorRepair(ctx, request)
		}
	case "vector-repair-record":
		if options.placement != PlacementKB || request.ID <= 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		embedded := EmbedRecord(ctx, invocation.TraceID, options.executor, options.data, request.ID, request.Command, request.Dimension)
		response.Embedding = &embedded

	case "maintenance-dashboard":
		backend, ok := options.data.(*postgresDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Payload, err = backend.maintenanceDashboard(ctx)

	case "memory-facts-claim", "memory-facts-complete":
		if options.placement != PlacementKB || invocation.PrincipalRef != 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		facts, ok := options.data.(memoryFactDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Operation == "memory-facts-claim" {
			response.FactWork, err = facts.ClaimMemoryFact(ctx)
		} else {
			if request.FactWork == nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = facts.CompleteMemoryFact(ctx, *request.FactWork, request.Content, request.Success, request.Reason)
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
			if store.IsNoRows(err) {
				err = nil
				break
			}
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
			if options.publicWrite && transaction == nil {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			id, err = legacy.GenerateEpisodeCard(ctx, request.SessionID)
			switch {
			case errors.Is(err, ErrMemoryNotFound):
				err = nil
			case errors.Is(err, errEpisodeDisabled):
				code := -3
				response.Code, err = &code, nil
			case errors.Is(err, errEpisodeCapacity):
				code := -4
				response.Code, err = &code, nil
			case errors.Is(err, errEpisodeMixedScope):
				code := -2
				response.Code = &code
				err = nil
			default:
				response.IDs = []int64{id}
			}
		case "vector-collection-exists":
			var exists bool
			exists, err = legacy.VectorCollectionExists(ctx)
			response.Allowed = &exists
		case "vector-collection-recreate":
			err = legacy.RecreateVectorCollection(ctx, request.Dimension)
			response.Updated = err == nil
		case "vector-search":
			if backend, ok := options.data.(*postgresDataStore); ok && explicitScope {
				response.VectorHits, err = backend.searchVectors(ctx, request.Vector, request.RecordType,
					request.Workspace, request.Project, request.IncludeAll, request.MaxResults, scope)
			} else {
				response.VectorHits, err = legacy.SearchVectors(ctx, request.Vector, request.RecordType,
					request.Workspace, request.Project, request.IncludeAll, request.MaxResults)
			}
		case "vector-rebuild":
			var rebuilt int
			if request.Version == "" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				if err := backend.db.QueryRow(ctx, `SELECT version FROM memory_active_embedder WHERE id=1`).Scan(&request.Version); err != nil || request.Version == "" {
					return nil, bus.ModuleStatusInvalidRequest
				}
			}
			response.Version = request.Version
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
	case "export-records", "export-decisions-jsonl", "export-jsonl":
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
			count, err = exporter.ExportJSONL(ctx, request.Path, request.Operation == "export-decisions-jsonl")
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
			if options.publicWrite && transaction == nil {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			request.Scope = scope
			var record Record
			if request.IdempotencyKey != "" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				record, response.MutationReceipt, err = backend.storeKBIdempotent(ctx, request, options.commandContext, strconv.FormatUint(invocation.TraceID, 10))
				rollbackOnly = err != nil
			} else {
				record, err = mutations.InsertEpistemic(ctx, request)
			}
			if err == nil && options.publicWrite && request.IdempotencyKey == "" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				err = backend.captureStoredFactActor(ctx, record.ID, request.Authority, options.commandContext)
			}
			response.Records = []Record{record}
		case "update-as":
			if request.ID <= 0 || request.Content == "" ||
				(request.Authority != AuthorityModel && request.Authority != AuthorityUser) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			if options.publicWrite && transaction == nil {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			var code int
			var newID int64
			if request.ExpectedVersion != nil || request.IdempotencyKey != "" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				authority := AuthorityModel
				if caller := options.commandContext; request.Authority == AuthorityUser && caller != nil && caller.Authenticated && caller.UserAuthority && caller.Principal != "" {
					authority = AuthorityUser
				}
				var record Record
				if request.IdempotencyKey != "" {
					request.Scope = scope
					record, response.MutationReceipt, err = backend.replaceKBIdempotent(ctx, request, authority, options.commandContext, strconv.FormatUint(invocation.TraceID, 10))
					rollbackOnly = err != nil
				} else {
					record, err = backend.replaceKBCorrection(ctx, request.ID, request.Content, nil, "", authority, nil, request.ExpectedVersion)
					if err == nil {
						err = backend.captureStoredFactActor(ctx, record.ID, authority, options.commandContext)
					}
				}
				newID = record.ID
			} else {
				code, newID, err = mutations.UpdateAs(ctx, request.ID, request.Content, request.Authority)
				if err == nil && code == MutationOK && options.publicWrite {
					backend := options.data.(*postgresDataStore)
					err = backend.captureStoredFactActor(ctx, newID, request.Authority, options.commandContext)
				}
			}
			if errors.Is(err, ErrMemoryNotFound) {
				code, err = -1, nil
			}
			response.Code = &code
			response.IDs = []int64{newID}
		case "delete-as":
			if request.ID <= 0 || (request.Authority != AuthorityModel && request.Authority != AuthorityUser) {
				return nil, bus.ModuleStatusInvalidRequest
			}
			if request.ExpectedVersion != nil {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				authority := AuthorityModel
				if caller := options.commandContext; request.Authority == AuthorityUser && caller != nil && caller.Authenticated && caller.UserAuthority && caller.Principal != "" {
					authority = AuthorityUser
				}
				if request.IdempotencyKey != "" {
					request.Scope = scope
					response.MutationReceipt, err = backend.deleteKBIdempotent(ctx, request, authority, options.commandContext, strconv.FormatUint(invocation.TraceID, 10))
					response.Deleted = err == nil
					rollbackOnly = err != nil
				} else {
					response.Deleted, err = backend.deleteKBVersion(ctx, request.ID, authority, request.ExpectedVersion)
				}
				if errors.Is(err, ErrMemoryNotFound) {
					err = nil
				}
			} else {
				response.Deleted, err = mutations.DeleteAs(ctx, request.ID, request.Authority)
			}
		}
	case "pii-inject":
		if request.Sensitivity < int(SensNormal) || request.Sensitivity > int(SensSecret) ||
			request.Confidence == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		allowed := ShouldInject(RelSensitivity(request.Sensitivity), *request.Confidence,
			request.TurnRequestsSensitive)
		response.Allowed = &allowed
	case "change-feed":
		// The feed carries private record identities. Only the embedding host
		// may consume it; it is not an advertised model/public diagnostic tool.
		if invocation.PrincipalRef != 0 || request.Changes == nil || !request.Changes.valid() {
			return nil, bus.ModuleStatusInvalidRequest
		}
		backend, ok := options.data.(*postgresDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		var page MemoryChangePage
		if options.placement == PlacementKB {
			if request.IncludeAll {
				return nil, bus.ModuleStatusInvalidRequest
			}
			page, err = backend.sharedChanges(ctx, scope, *request.Changes)
		} else {
			page, err = backend.personalChanges(ctx, *request.Changes)
		}
		response.Changes = &page
	case "get":
		if request.ID <= 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var record Record
		var getErr error
		if request.AtVersion != nil {
			backend, ok := options.data.(*postgresDataStore)
			if !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			record, getErr = backend.personalVersion(ctx, scope, *request.AtVersion)
		} else if request.AsOf != "" || (request.ReadPolicy != nil && request.ReadPolicy.Mode == "historical") {
			if options.placement != PlacementKB {
				return nil, bus.ModuleStatusInvalidRequest
			}
			backend, ok := options.data.(*postgresDataStore)
			if !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			when := ""
			if readResult != nil {
				when = readResult.ValidAt
			}
			record, getErr = backend.getAtVersioned(ctx, scope, request.ID, true, when, request.IncludeVersion)
		} else if request.IncludeVersion {
			backend, ok := options.data.(*postgresDataStore)
			if !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			record, getErr = backend.getAtVersioned(ctx, scope, request.ID, false, "", true)
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
	case "adaptive-search", "server-search":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Records, err = backend.adaptiveSearch(ctx, request)
		if err == nil && request.Operation == "server-search" {
			response.LegacyResults, err = backend.LegacySearch(ctx, request.Clusters, request.Limit)
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
		if backend, ok := options.data.(*postgresDataStore); ok {
			var record Record
			record, err = backend.upsertWorkflow(ctx, request)
			response.Records = []Record{record}
			break
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
		if request.IdempotencyKey != "" {
			backend, ok := options.data.(*postgresDataStore)
			if !ok || options.placement != PlacementServer {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			request.Scope, request.Confidence = scope, &confidence
			record, response.MutationReceipt, err = backend.mutatePersonalIdempotent(ctx, request, options.commandContext)
			rollbackOnly = err != nil
		} else {
			record, err = options.data.Put(ctx, scope, Record{Scope: scope, Tier: request.Tier,
				Kind: request.Kind, Key: request.Key, Content: request.Content, Confidence: confidence})
		}
		response.Records = []Record{record}
	case "supersede":
		if request.ID <= 0 || request.Content == "" || request.Confidence == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		advanced, ok := options.data.(dataAdvancedStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if options.publicWrite && transaction == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.ExpectedVersion != nil {
			if _, ok := options.data.(*postgresDataStore); !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
		}
		var record Record
		if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementKB {
			authority := AuthorityModel
			if caller := options.commandContext; request.Authority == AuthorityUser && caller != nil && caller.Authenticated && caller.UserAuthority && caller.Principal != "" {
				authority = AuthorityUser
			}
			if request.IdempotencyKey != "" {
				if transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				request.Scope = scope
				record, response.MutationReceipt, err = backend.replaceKBIdempotent(ctx, request, authority, options.commandContext, strconv.FormatUint(invocation.TraceID, 10))
				rollbackOnly = err != nil
			} else {
				record, err = backend.replaceKBVersion(ctx, request.ID, request.Content, *request.Confidence, request.SessionID, authority, nil, request.ExpectedVersion)
				if err == nil && options.publicWrite {
					err = backend.captureStoredFactActor(ctx, record.ID, authority, options.commandContext)
				}
			}
		} else if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementServer && request.ExpectedVersion != nil {
			if request.IdempotencyKey != "" {
				request.Scope = scope
				record, response.MutationReceipt, err = backend.mutatePersonalIdempotent(ctx, request, options.commandContext)
				rollbackOnly = err != nil
			} else {
				record, err = backend.correctPersonalVersion(ctx, scope, request.ID, request.Content, *request.Confidence, *request.ExpectedVersion)
			}
		} else {
			record, err = advanced.Supersede(ctx, scope, request.ID, request.Content, *request.Confidence)
		}
		switch {
		case errors.Is(err, ErrMemoryNotFound):
			err = nil
			response.Records = []Record{}
		case errors.Is(err, errImmutableExperience), errors.Is(err, errRequiresRevocation):
			code := MutationImmutableExperience
			if errors.Is(err, errRequiresRevocation) {
				code = MutationRequiresReplacement
			}
			response.Code, err = &code, nil
		default:
			response.Records = []Record{record}
		}
	case "feedback-path":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || invocation.PrincipalRef != 0 || options.placement != PlacementKB || transaction == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		err = backend.feedbackPath(ctx, request)
		response.Updated = err == nil
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
	case "prospective-current", "directive-current":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Operation == "prospective-current" {
			response.Prospectives, err = backend.prospectiveCurrent(ctx, request.Limit)
		} else {
			response.Directives, err = backend.recallOpenDirectives(ctx, request.Limit)
		}
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
		if request.CollectFactSources {
			projection, ok := options.data.(interface {
				RecallFactProjection(context.Context, string, string, bool, int) (string, int, *factProjection, error)
			})
			if !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			block, count, response.FactProjection, err = projection.RecallFactProjection(ctx, request.Entity, request.Query, request.TurnRequestsSensitive, request.ContentCapacity)
		} else {
			block, count, err = recall.RecallFacts(ctx, request.Entity, request.Query,
				request.TurnRequestsSensitive, request.ContentCapacity)
		}
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
			if creator, ok := directives.(interface {
				DirectiveCreateWithOutcome(context.Context, DataRequest) (Directive, bool, error)
			}); ok {
				item, response.Deduplicated, err = creator.DirectiveCreateWithOutcome(ctx, request)
			} else {
				item, err = directives.DirectiveCreate(ctx, request)
			}
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
			var id int64
			id, err = domain.UpdateContent(ctx, request.ID, request.Content)
			response.Updated = err == nil && id > 0
			if response.Updated {
				response.IDs = []int64{id}
			}
		case "reject":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			if request.IdempotencyKey != "" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				response.MutationReceipt, err = backend.lifecycleKBIdempotent(ctx, request, options.commandContext, strconv.FormatUint(invocation.TraceID, 10))
				response.Updated = err == nil
				rollbackOnly = err != nil
			} else if request.ExpectedVersion != nil {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				err = backend.lockKBLifecycleVersion(ctx, request.ID, request.ExpectedVersion)
			}
			if err == nil && request.IdempotencyKey == "" {
				response.Updated, err = domain.Reject(ctx, request.ID, request.Reason)
			}
			if errors.Is(err, ErrMemoryNotFound) {
				err = nil
			}
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
			target := scope
			if request.TagScope != nil {
				target, err = normalizeScope(options.placement, *request.TagScope)
				if err != nil {
					return nil, bus.ModuleStatusInvalidRequest
				}
			}
			if request.ID <= 0 || (request.TagScope == nil && request.Scope.Type == "") {
				return nil, bus.ModuleStatusInvalidRequest
			}
			response.Updated, err = domain.ScopeTag(ctx, request.ID, target)
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
			if errors.Is(err, ErrMemoryNotFound) {
				err = nil
				response.Episodes = []Episode{}
			} else if err == nil {
				response.Episodes = []Episode{item}
			}
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
			if errors.Is(err, ErrMemoryNotFound) {
				err = nil
			} else if err == nil {
				response.EntityProfile = &profile
			}
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
	case "stats-dashboard":
		dashboard, ok := options.data.(interface {
			DashboardStats(context.Context) (json.RawMessage, error)
		})
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		response.Payload, err = dashboard.DashboardStats(ctx)
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
			if request.IdempotencyKey != "" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				response.MutationReceipt, err = backend.lifecycleKBIdempotent(ctx, request, options.commandContext, strconv.FormatUint(invocation.TraceID, 10))
				response.Updated = err == nil
				rollbackOnly = err != nil
			} else if request.ExpectedVersion != nil {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil || !options.publicWrite {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				err = backend.lockKBLifecycleVersion(ctx, request.ID, request.ExpectedVersion)
			}
			if err == nil && request.IdempotencyKey == "" {
				response.Updated, err = queries.Restore(ctx, request.ID, request.Actor)
			}
			if errors.Is(err, ErrMemoryNotFound) {
				err = nil
			}
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
	case "compose-recall":
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementServer || invocation.PrincipalRef != 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		response.Payload, err = backend.ComposeRecall(ctx, request.SharedRecall, request.LimitTokens, request.SessionStart)
	case "recall-bundle", "briefing-bundle", "alerts-bundle", "assemble-context", "context-block", "context-ingress",
		"diagnose", "explain", "ask":
		if options.placement != PlacementKB && request.Operation != "recall-bundle" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		retrieval, ok := options.data.(retrievalDataStore)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch request.Operation {
		case "recall-bundle":
			if activated, ok := retrieval.(interface {
				RecallBundleWithActivation(context.Context, string, int, bool, json.RawMessage) (json.RawMessage, error)
			}); ok {
				response.Payload, err = activated.RecallBundleWithActivation(ctx, request.Query, request.LimitTokens, request.SessionStart, request.Activation)
			} else {
				if options.placement == PlacementKB && parseActivation(request.Activation) != nil {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				response.Payload, err = retrieval.RecallBundle(ctx, request.Query, request.LimitTokens, request.SessionStart)
			}
		case "briefing-bundle":
			response.Payload, err = retrieval.BriefingBundle(ctx, request.LimitTokens)
		case "alerts-bundle":
			response.Payload, err = retrieval.AlertsBundle(ctx, request.AsOf)
		case "assemble-context", "context-block", "context-ingress":
			var block string
			if request.Operation == "context-ingress" {
				backend, ok := options.data.(*postgresDataStore)
				if !ok || transaction == nil {
					return nil, bus.ModuleStatusCapabilityAbsent
				}
				response.Reason, err = backend.retractContextQuery(ctx, request.Query)
				if err != nil {
					break
				}
			}
			if request.Operation == "assemble-context" {
				var records []Record
				if backend, ok := options.data.(*postgresDataStore); ok && !explicitScope {
					records, err = backend.SearchVisible(ctx, request)
				} else {
					records, err = options.data.Search(ctx, scope, request.Query, "", "", request.Limit)
				}
				if request.Detail {
					assembly := assembleMemoryContextWithBudget(records, request.Query, request.BlockType, request.assemblyBytes)
					block = assembly.Context
					response.ContextAssembly = &assembly
				} else {
					// Native hosts need only the projection. Avoid computing
					// diagnostic scores/metadata for every unused candidate.
					block, _ = renderMemoryContextBounded(records, request.BlockType, request.assemblyBytes)
				}
			} else if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementKB && !explicitScope {
				var records []Record
				records, err = backend.SearchVisible(ctx, request)
				block = renderMemoryContext(records, request.BlockType)
			} else {
				block, err = retrieval.AssembleContext(ctx, scope, request.Query, request.BlockType, request.Limit)
			}
			if err == nil && request.Operation == "context-ingress" && request.Query != "" {
				var facts string
				facts, _, err = options.data.(*postgresDataStore).RecallFacts(ctx, "", request.Query, false, 2048)
				if facts != "" {
					block += "\n## Known facts\n" + facts
				}
			}
			response.Block = &block
		case "diagnose":
			ctx = context.WithValue(ctx, rankingTraceKey{}, !request.IngressPreview)
			if backend, ok := options.data.(*postgresDataStore); ok && options.placement == PlacementKB && !explicitScope {
				var records []Record
				records, err = backend.SearchVisible(ctx, request)
				for _, record := range records {
					response.Diagnostics = append(response.Diagnostics, diagnosticFor(record, request.Query))
				}
			} else {
				response.Diagnostics, err = retrieval.Diagnose(ctx, scope, request.Query, request.Limit)
			}
		case "explain":
			if request.ID <= 0 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var diagnostic Diagnostic
			diagnostic, err = retrieval.Explain(ctx, scope, request.Query, request.ID)
			response.Diagnostics = []Diagnostic{diagnostic}
		case "ask":
			var answer AnswerResult
			if backend, ok := options.data.(*postgresDataStore); ok {
				if explicitScope {
					request.Scope = scope
				}
				answer, err = backend.askRequest(ctx, request)
			} else {
				answer, err = retrieval.Ask(ctx, scope, request.Query, request.Limit)
			}
			response.Answer = &answer
		}
	case "delete":
		if request.ID <= 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if options.placement == PlacementServer && request.ExpectedVersion != nil {
			backend, ok := options.data.(*postgresDataStore)
			if !ok {
				return nil, bus.ModuleStatusCapabilityAbsent
			}
			if request.IdempotencyKey != "" {
				request.Scope = scope
				_, response.MutationReceipt, err = backend.mutatePersonalIdempotent(ctx, request, options.commandContext)
				rollbackOnly = err != nil
			} else {
				_, err = backend.retirePersonalVersion(ctx, scope, request.ID, *request.ExpectedVersion)
			}
			response.Deleted = err == nil
			if errors.Is(err, ErrMemoryNotFound) {
				err = nil
			}
		} else {
			response.Deleted, err = options.data.Delete(ctx, scope, request.ID)
		}
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	if code := mutationRefusal(err); code != 0 {
		proposal := proposedCorrection(err)
		response = DataResponse{Code: &code, Proposal: proposal}
		// Canonical admission has not written a version. The linked draft and
		// its audit/retry reference are the successful outcome of this request.
		if proposal != nil {
			rollbackOnly = false
		}
		err = nil
	}

	if err != nil {
		if invocation.Cancelled() || ctx.Err() != nil {
			return nil, bus.ModuleStatusCancelled
		}

		var budgetRefusal *contextBudgetError
		if (request.Operation == "recall-bundle" || request.Operation == "compose-recall") && errors.As(err, &budgetRefusal) {
			payload, _ := json.Marshal(commandError(budgetRefusal.kind, budgetRefusal.message))
			raw, _ := json.Marshal(DataResponse{Payload: payload})
			return raw, bus.ModuleStatusOK // transaction rolls back; no surfaced counters
		}

		if request.Operation == "fact-retract" {
			reason, message := "", ""
			switch {
			case errors.Is(err, errFactImmutable):
				reason, message = "immutable", "immutable facts require verified user authority"
			case errors.Is(err, errFactAnnotateOnly):
				reason, message = "annotate_only", "historical facts may only be annotated"
			case errors.Is(err, errFactOperatorOnly):
				reason, message = "operator_required", "policy facts require operator authority"
			}
			if reason != "" {
				payload, _ := json.Marshal(map[string]any{"status": "error", "kind": "conflict", "reason": reason, "message": message})
				raw, _ := json.Marshal(DataResponse{Payload: payload})
				return raw, bus.ModuleStatusOK
			}
		}
		if (request.Operation == "entity-review" || request.Operation == "entity-mutate") && errors.Is(err, errEntityTransition) {
			kind := "conflict"
			if request.Operation == "entity-mutate" {
				kind = "not_found"
			}
			payload, _ := json.Marshal(commandError(kind, "entity transition refused: unknown, inactive, already undone, or no longer current"))
			raw, _ := json.Marshal(DataResponse{Payload: payload})
			return raw, bus.ModuleStatusOK
		}
		if request.Operation == "fact-review" {
			kind := ""
			if errors.Is(err, ErrMemoryNotFound) {
				kind = "not_found"
			} else if errors.Is(err, errFactReviewConflict) || errors.Is(err, errFactTombstoned) {
				kind = "conflict"
			}
			if kind != "" {
				payload, _ := json.Marshal(commandError(kind, "fact review transition refused"))
				raw, _ := json.Marshal(DataResponse{Payload: payload})
				return raw, bus.ModuleStatusOK
			}
		}
		return nil, bus.ModuleStatusInternal
	}
	if request.PublicView {
		backend, ok := options.data.(*postgresDataStore)
		if !ok || options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if request.Operation == "diagnose" || request.Operation == "explain" {
			for i := range response.Diagnostics {
				d := &response.Diagnostics[i]
				response.Records = append(response.Records, d.Memory)
				if request.IngressPreview {
					continue
				}
				d.EpistemicKind, err = backend.EpistemicKind(ctx, d.Memory.ID)
				if err != nil {
					return nil, bus.ModuleStatusInternal
				}
			}
		}
		if request.IngressPreview {
			exact := Scope{}
			if explicitScope {
				exact = request.Scope
			}
			response.MemoryPreviews, response.PreviewProjection, err = backend.ingressMemoryPreviews(ctx, response.Diagnostics, exact)
		} else {
			response.PublicRecords, err = backend.publicRecords(ctx, response.Records)
		}
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		if request.Operation == "get" && request.AsOf != "" && len(response.Records) > 0 {
			valid, validErr := backend.ValidAt(ctx, request.ID, request.AsOf)
			if validErr == nil {
				response.ValidAt = &valid
			}
		}
	}
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if transaction != nil && !rollbackOnly {
		if err = transaction.Commit(ctx); err != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	return encoded, bus.ModuleStatusOK
}

// Only fixed categories and validated SQLSTATEs may cross the diagnostic boundary.
func memoryFailureClass(err error) string {
	if errors.Is(err, context.DeadlineExceeded) {
		return "deadline"
	}
	if errors.Is(err, context.Canceled) {
		return "cancelled"
	}
	var wireError *store.StoreError
	var sqlError interface{ SQLState() string }
	code := ""
	if errors.As(err, &wireError) {
		code = wireError.SQLState
	} else if errors.As(err, &sqlError) {
		code = sqlError.SQLState()
	}
	if len(code) == 5 {
		valid := true
		for _, c := range code {
			valid = valid && (c >= '0' && c <= '9' || c >= 'A' && c <= 'Z')
		}
		if valid {
			return "sqlstate_" + code
		}
	}
	switch {
	case errors.Is(err, store.ErrStoreUnavailable):
		return "store_unavailable"
	case errors.Is(err, store.ErrResultTooLarge):
		return "result_capacity"
	case errors.Is(err, store.ErrTxClosed):
		return "transaction_closed"
	}
	return "internal"
}

// Unknown input must not become diagnostic text if opening the store fails
// before the operation dispatcher rejects it.
func memoryFailureOperation(operation string) string {
	switch operation {
	case "get", "store", "insert-epistemic", "supersede", "update-as", "delete", "delete-as",
		"correction-review", "recall-bundle", "compose-recall", "typed-context", "assertion-search",
		"episode-list", "episode-get", "relation-search", "entity-edges", "entity-profile",
		"rebuild-derived", "code-index", "change-feed", "vector-search", "vector-rebuild":
		return operation
	default:
		return "other"
	}
}
