package roundtable

// Chunk planning and synthesis assembly for the authoring pipeline.
//
// Moved from C (src/modules/roundtable/roundtable_pipeline_chunk.c). Deciding
// where a large review artifact is split, and which spans fit the synthesis
// budget, is policy over its arguments: no state, no I/O.
//
// What did NOT move is the hashing. rtp_chunk_hash is a general content-identity
// primitive used in four other places in the pipeline, so implementing FNV-1a
// here as well would put the same hash in two languages with nothing keeping
// them equal -- and chunk hashes are compared against re-derived ones, so a
// divergence would surface as spurious staleness. The split is therefore
// boundaries here, hashing in C: the module returns offsets and lengths, and the
// caller fills in the digests with the implementation it already has.
//
// The design invariant the C header states is preserved: chunks are DERIVED from
// the retained whole origin, never a separate durable store, so re-deriving each
// pass gives per-pass freshness for free.

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

// MaxChunks mirrors RTP_MAX_CHUNKS. Running out of slots sets Truncated rather
// than growing the plan, so the caller can surface a coverage gap.
const MaxChunks = 64

type ChunkSpan struct {
	Index  int `json:"index"`
	Offset int `json:"offset"`
	Len    int `json:"len"`
}

type ChunkPlan struct {
	Chunks      []ChunkSpan `json:"chunks"`
	Count       int         `json:"count"`
	BudgetBytes int         `json:"budget_bytes"`
	OriginLen   int         `json:"origin_len"`
	// OverBudget: a single indivisible line exceeded the budget, so a hard cut
	// was taken. Truncated: the origin needed more than MaxChunks spans.
	OverBudget bool `json:"over_budget"`
	Truncated  bool `json:"truncated"`
}

type Assembly struct {
	Selected      []int `json:"selected"`
	SelectedCount int   `json:"selected_count"`
	Omitted       []int `json:"omitted"`
	OmittedCount  int   `json:"omitted_count"`
	BudgetBytes   int   `json:"budget_bytes"`
	UsedBytes     int   `json:"used_bytes"`
	OverBudget    bool  `json:"over_budget"`
}

type ChunkPlanRequest struct {
	Origin      string `json:"origin"`
	BudgetBytes int    `json:"budget_bytes"`
	// AssemblyBudget is the synthesis budget. The caller plans and assembles back
	// to back, so both answers come from one round trip rather than two.
	AssemblyBudget int `json:"assembly_budget"`
}

type ChunkPlanResponse struct {
	Plan     ChunkPlan `json:"plan"`
	Assembly Assembly  `json:"assembly"`
}

// ChunkNeeded reports whether origin exceeds the budget at all.
func ChunkNeeded(origin string, budgetBytes int) bool {
	if budgetBytes <= 0 {
		return false
	}
	return len(origin) > budgetBytes
}

// PlanChunks splits origin into spans of at most budgetBytes, preferring to
// break on the last newline in the window so a chunk holds whole lines.
func PlanChunks(origin string, budgetBytes int) ChunkPlan {
	plan := ChunkPlan{
		BudgetBytes: budgetBytes,
		OriginLen:   len(origin),
		Chunks:      []ChunkSpan{},
	}

	// The whole artifact fits, or no budget is set: one span covering everything.
	if budgetBytes <= 0 || len(origin) <= budgetBytes {
		plan.Chunks = append(plan.Chunks, ChunkSpan{Index: 0, Offset: 0, Len: len(origin)})
		plan.Count = 1
		return plan
	}

	pos := 0
	for pos < len(origin) && plan.Count < MaxChunks {
		remaining := len(origin) - pos
		take := budgetBytes
		if remaining < budgetBytes {
			take = remaining
		}
		if take == budgetBytes {
			brk := -1
			for i := pos + take - 1; i > pos; i-- {
				if origin[i] == '\n' {
					brk = i
					break
				}
			}
			if brk > pos {
				take = brk - pos + 1 // include the newline
			} else {
				// A single line longer than the budget cannot be split on a
				// boundary, so take a hard cut and say so.
				plan.OverBudget = true
			}
		}
		plan.Chunks = append(plan.Chunks, ChunkSpan{Index: plan.Count, Offset: pos, Len: take})
		plan.Count++
		pos += take
	}
	if pos < len(origin) {
		plan.Truncated = true // ran out of slots
	}
	return plan
}

// BuildAssembly greedily selects spans whose cumulative size fits budgetBytes;
// the remainder are recorded as a coverage gap rather than dropped silently.
func BuildAssembly(plan ChunkPlan, budgetBytes int) Assembly {
	a := Assembly{BudgetBytes: budgetBytes, Selected: []int{}, Omitted: []int{}}
	for _, c := range plan.Chunks {
		if budgetBytes > 0 && c.Len > budgetBytes && a.SelectedCount == 0 {
			// The very first span already overflows the synthesis budget.
			a.OverBudget = true
		}
		if budgetBytes <= 0 || a.UsedBytes+c.Len <= budgetBytes {
			a.Selected = append(a.Selected, c.Index)
			a.SelectedCount++
			a.UsedBytes += c.Len
		} else {
			a.Omitted = append(a.Omitted, c.Index)
			a.OmittedCount++
		}
	}
	return a
}

func encodeChunkPlan(v ChunkPlanResponse) ([]byte, error) { return json.Marshal(v) }

// handleChunkPlan serves StageChunkPlan. Plan and assembly come back together
// because the caller does both back to back on the same artifact, so splitting
// them would cost a second round trip carrying the artifact again.
func handleChunkPlan(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded ChunkPlanRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	plan := PlanChunks(decoded.Origin, decoded.BudgetBytes)
	encoded, err := encodeChunkPlan(ChunkPlanResponse{
		Plan:     plan,
		Assembly: BuildAssembly(plan, decoded.AssemblyBudget),
	})
	if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
