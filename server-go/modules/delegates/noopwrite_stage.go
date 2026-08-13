package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Did a write delegate that reported success actually change anything?
//
// The caller gathers the evidence -- file snapshots, `git status`, a HEAD
// comparison -- because all three are I/O. Only the reading of it is here.

const (
	StageNoopWrite uint32 = 18
	EventNoopWrite uint32 = 6674

	noopWriteRequestMagic  uint32 = 0x51574e44 /* "DNWQ" */
	noopWriteResponseMagic uint32 = 0x53574e44 /* "DNWS" */
	noopWriteReqLen               = 16

	// Bit 0 was IsWriteRole, which said the same thing as this one.
	noopFlagWritesAllowed uint32 = 1 << 1
	noopFlagHandoffJSON   uint32 = 1 << 2
	noopFlagSucceeded     uint32 = 1 << 3
	noopFlagAnyNamed      uint32 = 1 << 4
	noopFlagWorktreeDirty uint32 = 1 << 5
	noopFlagHeadAdvanced  uint32 = 1 << 6
	noopFlagHeadSnapshot  uint32 = 1 << 7
	noopFlagHasWorktree   uint32 = 1 << 8

	noopFlagsKnown = noopFlagWritesAllowed | noopFlagHandoffJSON |
		noopFlagSucceeded | noopFlagAnyNamed | noopFlagWorktreeDirty | noopFlagHeadAdvanced |
		noopFlagHeadSnapshot | noopFlagHasWorktree
)

func handleNoopWrite(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != noopWriteReqLen ||
		binary.LittleEndian.Uint32(request[0:4]) != noopWriteRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	flags := binary.LittleEndian.Uint32(request[8:12])
	if flags&^noopFlagsKnown != 0 {
		// An unknown flag means the caller believes it is sending something
		// this module does not read, and a judgement made on a partial view of
		// the evidence is worse than none.
		return nil, bus.ModuleStatusInvalidRequest
	}
	namedCount := int(int32(binary.LittleEndian.Uint32(request[12:16])))
	if namedCount < 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	verdict := JudgeNoopWrite(NoopWriteEvidence{
		WritesAllowed:     flags&noopFlagWritesAllowed != 0,
		HandoffJSON:       flags&noopFlagHandoffJSON != 0,
		Succeeded:         flags&noopFlagSucceeded != 0,
		NamedPathCount:    namedCount,
		AnyNamedChanged:   flags&noopFlagAnyNamed != 0,
		WorktreeChanged:   flags&noopFlagWorktreeDirty != 0,
		HeadAdvanced:      flags&noopFlagHeadAdvanced != 0,
		HeadSnapshotTaken: flags&noopFlagHeadSnapshot != 0,
		HasWorktree:       flags&noopFlagHasWorktree != 0,
	})

	// The message travels with the verdict: the benign cases are the ones an
	// operator most needs the wording for, because nothing failed and the only
	// evidence the guard ran at all is what it said.
	response := make([]byte, 12, 12+len(verdict.Message))
	binary.LittleEndian.PutUint32(response[0:4], noopWriteResponseMagic)
	putBool(response[4:8], verdict.Noop)
	putBool(response[8:12], verdict.Benign)
	response = append(response, verdict.Message...)
	return response, bus.ModuleStatusOK
}
