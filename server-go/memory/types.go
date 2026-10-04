package memory

import "encoding/json"

// NodeKind is an entity kind. Values match memory_node_kind_t exactly; they are
// persisted as integer codes, so they are assigned rather than derived.
type NodeKind uint32

const (
	NodeFile      NodeKind = 0
	NodeFunction  NodeKind = 1
	NodeStruct    NodeKind = 2
	NodeModule    NodeKind = 3
	NodeBug       NodeKind = 4
	NodeCommit    NodeKind = 5
	NodePr        NodeKind = 6
	NodeDeveloper NodeKind = 7
	NodeConcept   NodeKind = 8
	NodeEvent     NodeKind = 9
	NodePerson    NodeKind = 10
	NodePlace     NodeKind = 11
	NodeTimeExpr  NodeKind = 12
	NodeDevice    NodeKind = 13
	NodeOrg       NodeKind = 14
	NodeIp        NodeKind = 15
	NodeScalar    NodeKind = 16
	// NodeOther is the ANY wildcard when it appears in a kind list.
	NodeOther NodeKind = 99
)

// FactVerdict mirrors fact_gate_verdict_t. Only the values the pure gate can
// return are defined here; DEFER and REJECT_SENSITIVE belong to the DB-backed
// commit path in core and are never produced by this stage.
type FactVerdict uint32

const (
	// FactAccept means a known relation whose ends satisfy its kind constraints.
	FactAccept FactVerdict = 0
	// FactRejectKind means a known relation used with a disallowed end kind.
	FactRejectKind FactVerdict = 1
	// FactNovel means the relation is not in the seed ontology; the caller
	// consults the live table and stages or defers.
	FactNovel FactVerdict = 2
	// FactBadArg means no relation was supplied.
	FactBadArg FactVerdict = 3
)

// RelSensitivity is a relation's PII gating tier. Values match
// rel_sensitivity_t; they are persisted as text and compared as integers, so
// the numbering is assigned rather than derived.
type RelSensitivity uint32

const (
	// SensNormal is an identity or operational fact: injected above the
	// confidence floor.
	SensNormal RelSensitivity = 0
	// SensPII is a regulated identifier: injected only when the turn asks for it.
	SensPII RelSensitivity = 1
	// SensSecret is a credential: never injected, served through the vault.
	SensSecret RelSensitivity = 2
)

// Triple is a candidate fact found before the model. RelType is a normalized
// guess; the gate still decides whether it is written and how.
type Triple struct {
	Subject     string
	RelType     string
	Object      string
	SubjectKind NodeKind
	ObjectKind  NodeKind
}

// Command is one declared verb. The CLI spelling (`aimee memory get`), the RPC
// spelling ("memory.get") and the MCP spelling (tool `memory`, command=get) are
// all derived from Group+Verb -- not maintained separately, which is how
// memory.recall became memory_recall while memory.search became search_memory,
// verb first, leaving no mechanical mapping between the surfaces.
type Command struct {
	Group      string
	Verb       string
	Summary    string
	Surfaces   uint32
	Visibility uint32
}

type EmbedRequest struct {
	Operation string   `json:"operation,omitempty"`
	MemoryID  int64    `json:"memory_id,omitempty"`
	BaseURL   string   `json:"base_url"`
	InputType string   `json:"input_type"`
	Text      string   `json:"text"`
	Texts     []string `json:"texts,omitempty"`
	MaxDim    int      `json:"max_dim"`
	Limit     int      `json:"limit,omitempty"`
	NowMS     int64    `json:"now_ms,omitempty"`
}

// EmbedResponse separates the ways this can decline, because they are different
// facts and a caller that conflates them misreports the embedder's health:
//
//	Unavailable  the breaker suppressed the call; nothing was sent
//	Unauthorized the service was REACHED and refused us (401/403)
//	Error        the call was attempted and failed
//
// Unauthorized is the subtle one: it proves reachability, so it must not count
// as a failure — and it closes an earlier outage, or a half-open breaker would
// turn the next authorization result back into "unavailable".
type EmbedResponse struct {
	Vectors                 [][]float32        `json:"vectors,omitempty"`
	Vector                  []float32          `json:"vector,omitempty"`
	Dim                     int                `json:"dim"`
	Truncated               bool               `json:"truncated,omitempty"`
	Unavailable             bool               `json:"unavailable,omitempty"`
	RetryAfterMS            int64              `json:"retry_after_ms,omitempty"`
	Unauthorized            bool               `json:"unauthorized,omitempty"`
	Error                   string             `json:"error,omitempty"`
	ServingID               string             `json:"serving_id,omitempty"`
	IdentityState           string             `json:"identity_state,omitempty"`
	EmbeddingIdentity       *EmbeddingIdentity `json:"embedding_identity,omitempty"`
	EmbeddingIdentityDigest string             `json:"embedding_identity_digest,omitempty"`
	Embedded                bool               `json:"embedded,omitempty"`
	Repaired                int                `json:"repaired,omitempty"`
	Failed                  int                `json:"failed,omitempty"`
}

// EmbedIsHTTP reports whether a configured embedder command names an HTTP
// endpoint rather than a program to run.
// FactWriteRequest preserves the gate's relation spelling and bounded wire
// contract. General data-operation relation normalization must not change it.
type FactWriteRequest struct {
	Head     NodeKind `json:"head"`
	Relation string   `json:"relation"`
	Tail     NodeKind `json:"tail"`
}

// FactWriteDecision keeps the ontology answer separate from commit eligibility:
// observe-only callers still need the former for a relation withheld from KB.
// The Go owner makes both decisions in one invocation, with no native PII
// classifier or second policy round trip between validation and commit.
type FactWriteDecision struct {
	Verdict       FactVerdict `json:"verdict"`
	CommitAllowed bool        `json:"commit_allowed"`
}

func (d *FactWriteDecision) UnmarshalJSON(body []byte) error {
	var fields struct {
		Verdict       *FactVerdict `json:"verdict"`
		CommitAllowed *bool        `json:"commit_allowed"`
	}
	if err := json.Unmarshal(body, &fields); err != nil {
		return err
	}
	if fields.Verdict == nil || fields.CommitAllowed == nil {
		return ErrClientResponse
	}
	d.Verdict, d.CommitAllowed = *fields.Verdict, *fields.CommitAllowed
	return nil
}
