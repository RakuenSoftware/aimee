package workflows

// The fail-closed roundtable gate decision rule.
//
// Moved from C (src/modules/workflows/wfe_verdict.c). It is pure policy over a
// panel's verdicts -- no state, no I/O -- and it is evaluated once per gate, not
// per verdict, so it crosses the bus as one call rather than N.
//
// Every rule here is load-bearing and several encode a specific past failure.
// They are preserved exactly; see the comments on each.

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

type VerdictKind int

const (
	VerdictApprove VerdictKind = iota
	VerdictRequestChanges
	VerdictComment
	VerdictMalformed // unparseable/empty -> fails closed to request_changes
)

type GateDecision string

const (
	GateApprove  GateDecision = "approve"  // -> ADVANCED
	GateChanges  GateDecision = "changes"  // -> LOOPED (re-author/re-implement)
	GateDegraded GateDecision = "degraded" // -> PENDING (panel could not be composed)
)

// VerdictSchema is the only schema version this rule trusts. A verdict carrying
// anything else is not merely ignored, it is coerced to request_changes.
const VerdictSchema = 1

// Verdict carries only what the DECISION reads. The C struct also holds the
// panelist's model and critique text, but neither participates in the ruling --
// the critique is threaded back to the re-authoring delegate separately -- so
// they stay off the wire rather than shipping a kilobyte per verdict.
type Verdict struct {
	Persona             string      `json:"persona"`
	SchemaVersion       int         `json:"schema_version"`
	ReviewedContentHash string      `json:"reviewed_content_hash"`
	Kind                VerdictKind `json:"kind"`
	HighSevBlockers     int         `json:"high_sev_blockers"`
}

type GateDecideRequest struct {
	Verdicts     []Verdict `json:"verdicts"`
	Required     []string  `json:"required"`
	Quorum       int       `json:"quorum"`
	ArtifactHash string    `json:"artifact_hash"`
}

type GateDecideResponse struct {
	Decision GateDecision `json:"decision"`
	Reason   string       `json:"reason"`
}

// structurallyValid: a verdict is trustworthy only if its schema is known, it is
// not malformed, and it reviewed the artifact actually under review.
func structurallyValid(v Verdict, artifactHash string) bool {
	if v.SchemaVersion != VerdictSchema {
		return false
	}
	if v.Kind == VerdictMalformed {
		return false
	}
	if artifactHash != "" && v.ReviewedContentHash != artifactHash {
		return false
	}
	return true
}

// effectiveKind coerces an untrustworthy verdict to request_changes, so a
// tampered or malformed response can never approve.
func effectiveKind(v Verdict, artifactHash string) VerdictKind {
	if structurallyValid(v, artifactHash) {
		return v.Kind
	}
	return VerdictRequestChanges
}

// EffectiveQuorum is max(requested, nreq, 2) -- the single-lens floor.
func EffectiveQuorum(requested, nreq int) int {
	q := requested
	if q < nreq {
		q = nreq
	}
	if q < 2 {
		q = 2
	}
	return q
}

// GateDecide returns the ruling and a human-readable reason.
//
// quorum is the already-floored effective quorum; the caller applies
// EffectiveQuorum.
func GateDecide(verdicts []Verdict, required []string, quorum int, artifactHash string) GateDecideResponse {
	// Required coverage: every required persona must have returned a
	// STRUCTURALLY VALID verdict. A malformed / wrong-schema / hash-mismatched
	// response from a required persona is an integrity failure, NOT coverage.
	// Treating its mere presence as satisfied would let the gate approve on other
	// panelists' quorum while a required lens never reviewed the artifact, so it
	// fails closed to DEGRADED exactly as if that persona were absent.
	for _, want := range required {
		present := false
		for _, v := range verdicts {
			if v.Persona == want && structurallyValid(v, artifactHash) {
				present = true
				break
			}
		}
		if !present {
			return GateDecideResponse{
				Decision: GateDegraded,
				Reason:   "required persona '" + want + "' missing or untrustworthy verdict",
			}
		}
	}

	// Quorum counts NON-BLOCKING verdicts: approve and comment. A comment is by
	// definition "non-blocking remarks only". Requiring literal approve from
	// every lens made a required-4/quorum-4 gate practically unpassable for an
	// autonomous run: cautious reviewers emit comment, the gate loops to its
	// attempt cap, and every run parks pending_human.
	//
	// Fail-closed is preserved where it matters: ANY effective request_changes
	// loops (malformed/tampered coerce to it), any high-severity blocker loops,
	// and at least one lens must still explicitly approve -- comments alone never
	// pass a gate.
	approve, highSev, changes, nonBlocking := 0, 0, 0, 0
	for _, v := range verdicts {
		// Only trust blocker counts from structurally-valid verdicts; an
		// untrustworthy one already fails the gate via effectiveKind.
		if structurallyValid(v, artifactHash) && v.HighSevBlockers > 0 {
			highSev += v.HighSevBlockers
		}
		switch effectiveKind(v, artifactHash) {
		case VerdictApprove:
			approve++
			nonBlocking++
		case VerdictComment:
			nonBlocking++
		default:
			changes++
		}
	}

	switch {
	case highSev > 0:
		return GateDecideResponse{
			Decision: GateChanges,
			Reason:   itoa(highSev) + " unresolved high-severity blocker(s)",
		}
	case changes > 0:
		return GateDecideResponse{
			Decision: GateChanges,
			Reason:   itoa(changes) + " reviewer(s) requested changes",
		}
	case approve < 1 || nonBlocking < quorum:
		return GateDecideResponse{
			Decision: GateChanges,
			Reason: "approve quorum not met (approve=" + itoa(approve) +
				", non-blocking=" + itoa(nonBlocking) + "/" + itoa(quorum) + ")",
		}
	}
	return GateDecideResponse{
		Decision: GateApprove,
		Reason: "approved (" + itoa(approve) + " approve + " + itoa(nonBlocking-approve) +
			" comment >= " + itoa(quorum) + ", no blockers)",
	}
}

// itoa avoids pulling fmt in for what is only ever a small non-negative count.
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		buf[i] = '-'
	}
	return string(buf[i:])
}

func decodeGateRequest(request []byte) (GateDecideRequest, bool) {
	var decoded GateDecideRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return decoded, false
	}
	return decoded, true
}

// handleGateDecide serves StageGateDecide.
//
// The ruling is fail-closed, so a request we cannot read is refused outright
// rather than answered with a default: an invented "approve" here would be a
// gate bypass, and an invented "changes" would silently loop a healthy run.
func handleGateDecide(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	decoded, ok := decodeGateRequest(request)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	encoded, err := json.Marshal(GateDecide(decoded.Verdicts, decoded.Required,
		decoded.Quorum, decoded.ArtifactHash))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
