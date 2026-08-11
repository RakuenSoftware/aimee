package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Judging whether a container the runtime just started is actually isolated.
//
// The caller runs the probe -- that is I/O, and inspecting a container is its
// job. What the report MEANS, and what to do about it, is decided here: reading
// the probe and judging it are one rule, because the whole difficulty is that
// "the probe failed" and "the sandbox is open" look the same from here.

const (
	StageIsolation uint32 = 14
	EventIsolation uint32 = 6670

	isolationRequestMagic  uint32 = 0x51534944 /* "DISQ" */
	isolationResponseMagic uint32 = 0x53534944 /* "DISS" */
	isolationReqHeaderLen         = 16

	isolationFlagProbeFailed uint8 = 1
	isolationFlagRequire     uint8 = 2

	isolationReportMax = 1 << 16
)

// handleIsolation reads the probe report and returns the verdict.
//
// The report arrives as content: the module never runs a container runtime.
func handleIsolation(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < isolationReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != isolationRequestMagic ||
		request[4] != wireVersion || request[5] > 3 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	probeFailed := request[5]&isolationFlagProbeFailed != 0
	requireIsolation := request[5]&isolationFlagRequire != 0

	reportLen := int(binary.LittleEndian.Uint32(request[8:12]))
	if reportLen > isolationReportMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	c := &economicsCursor{buf: request, at: isolationReqHeaderLen}
	report := c.str(reportLen)
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	verdict := JudgeIsolation(ParseIsolationProbe(report, probeFailed), requireIsolation)

	// The reason travels with the verdict. A caller that logged its own wording
	// would describe a judgement it did not make, and the wording is the part an
	// operator acts on.
	response := make([]byte, 16, 16+len(verdict.Reason))
	binary.LittleEndian.PutUint32(response[0:4], isolationResponseMagic)
	putBool(response[4:8], verdict.Refuse)
	putBool(response[8:12], verdict.Warn)
	// The SEVERITY too: a breach that runs anyway is an error, not a caution,
	// and a caller left to infer that from the wording would get it wrong.
	putBool(response[12:16], verdict.Error)
	response = append(response, verdict.Reason...)
	return response, bus.ModuleStatusOK
}
