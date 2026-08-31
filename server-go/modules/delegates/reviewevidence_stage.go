package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Did a review look at what it reviewed?
//
// The caller supplies the two facts this side cannot compute -- whether the
// review target came in the prompt, and whether the worktree is dirty -- and
// gets back what to check and what is already wrong.

const (
	StageReviewEvidence uint32 = 20
	EventReviewEvidence uint32 = 6676

	reviewEvidenceRequestMagic  uint32 = 0x51455244 /* "DREQ" */
	reviewEvidenceResponseMagic uint32 = 0x53455244 /* "DRES" */
	reviewEvidenceHeaderLen            = 16

	reviewFlagTargetProvided uint32 = 1 << 0
	reviewFlagWorktreeDirty  uint32 = 1 << 1

	reviewFlagsKnown = reviewFlagTargetProvided | reviewFlagWorktreeDirty

	reviewVerdictGuarded       uint32 = 1 << 0
	reviewVerdictCheckSnippets uint32 = 1 << 1
	reviewVerdictContradiction uint32 = 1 << 2

	reviewRoleMax = 64
)

func handleReviewEvidence(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < reviewEvidenceHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != reviewEvidenceRequestMagic ||
		request[4] != wireVersion || request[5] != 0 || request[6] != 0 || request[7] != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}

	flags := binary.LittleEndian.Uint32(request[8:12])
	if flags&^reviewFlagsKnown != 0 {
		// An unknown flag means the caller believes it is sending a fact this
		// module reads. Judging the evidence while ignoring part of it is worse
		// than not judging it.
		return nil, bus.ModuleStatusInvalidRequest
	}

	roleLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if roleLen < 0 || roleLen > reviewRoleMax || reviewEvidenceHeaderLen+roleLen > len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	role := string(request[reviewEvidenceHeaderLen : reviewEvidenceHeaderLen+roleLen])
	response := string(request[reviewEvidenceHeaderLen+roleLen:])

	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	v := JudgeReviewEvidence(ReviewEvidenceFacts{
		Role:           role,
		Response:       response,
		TargetProvided: flags&reviewFlagTargetProvided != 0,
		WorktreeDirty:  flags&reviewFlagWorktreeDirty != 0,
	})

	var verdict uint32
	if v.Guarded {
		verdict |= reviewVerdictGuarded
	}
	if v.CheckSnippets {
		verdict |= reviewVerdictCheckSnippets
	}
	if v.Contradiction {
		verdict |= reviewVerdictContradiction
	}

	out := make([]byte, 8+len(v.Error))
	binary.LittleEndian.PutUint32(out[0:4], reviewEvidenceResponseMagic)
	binary.LittleEndian.PutUint32(out[4:8], verdict)
	copy(out[8:], v.Error)
	return out, bus.ModuleStatusOK
}
