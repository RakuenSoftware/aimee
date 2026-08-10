package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Whether a verification run says anything about the delegate's WORK, or only
// about the machine it ran on.
//
// A verifier that ran and reported a build or test failure indicts the work
// product. A verifier that could not be run at all indicts the environment.
// Only the first is evidence about the model, and conflating them turns a
// missing binary or an OOM kill into "the model wasn't good enough".

const (
	StageVerify uint32 = 7
	EventVerify uint32 = 6663

	verifyRequestMagic  uint32 = 0x51524556 /* "VERQ" */
	verifyResponseMagic uint32 = 0x53524556 /* "VERS" */
	verifyRequestLen           = 20
	verifyResponseLen          = 12

	// Two questions, because the caller asks them at different moments: it
	// classifies as soon as the verifier returns, and asks about escalation
	// later, once it knows whether the delegate run itself finished.
	verifyOpClassify byte = 0
	verifyOpEscalate byte = 1
)

// VerifyOutcome mirrors verify_outcome_t.
type VerifyOutcome uint32

const (
	VerifyPass VerifyOutcome = 0
	// The verifier ran to completion and reported failure: an attributable
	// statement about the delegate's work product.
	VerifyFailed VerifyOutcome = 1
	// The verifier could not be run, or did not exit normally. Says nothing
	// about the work product.
	VerifyInfraError VerifyOutcome = 2
)

func (o VerifyOutcome) String() string {
	switch o {
	case VerifyPass:
		return "pass"
	case VerifyFailed:
		return "failed"
	case VerifyInfraError:
		return "infra_error"
	}
	return "unknown"
}

// ClassifyVerify reads the return of running a verifier under `/bin/sh -c`.
//
// maxSignalStatus is the highest status this PLATFORM can report as "killed by
// signal N" (128+SIGRTMAX on Linux, far lower without realtime signals). It is
// supplied by the caller rather than derived here: it is a property of the
// machine the verifier ran on, and picking a constant would misclassify a
// deliberate exit 160 as infrastructure wherever the real ceiling is lower.
//
// This is a HEURISTIC and cannot be made exact -- `sh -c` collapses "died from
// signal N" and "deliberately exited 128+N" into one integer. The tie breaks
// toward INFRA_ERROR because the two mistakes are not symmetric: reading a real
// test failure as infrastructure merely withholds a placement warning, while
// reading an OOM kill or a missing binary as a work-product failure blames the
// model for its environment. A verifier needing an unambiguous work-product
// failure should exit below 124.
func ClassifyVerify(execRC int32, maxSignalStatus int32) VerifyOutcome {
	if execRC == 0 {
		return VerifyPass
	}
	// safe_exec_capture returns -1 when it could not fork/exec, or when the
	// child shell did not exit normally.
	if execRC < 0 {
		return VerifyInfraError
	}
	//   126/127  shell's "not executable" / "not found"
	//   124      GNU coreutils `timeout` reporting expiry
	//   128+N    killed by signal N (137 = SIGKILL/OOM, 143 = SIGTERM)
	if execRC == 126 || execRC == 127 || execRC == 124 ||
		(execRC >= 129 && execRC <= maxSignalStatus) {
		return VerifyInfraError
	}
	return VerifyFailed
}

// VerifyEscalationWarranted reports whether this result is evidence the packet
// was placed on too weak a seat -- worth REPORTING so a human can consider a
// dearer retry. Nothing acts on it automatically.
//
// The claim is "this model was not good enough for this work", so it needs an
// attributable, verified work-product failure. A delegate run that did not
// finish is an availability problem for retry and failover; reporting it as a
// misplacement would blame the model for a transport or process failure it did
// not cause.
func VerifyEscalationWarranted(delegateRC int32, outcome VerifyOutcome) bool {
	if delegateRC != 0 {
		return false
	}
	return outcome == VerifyFailed
}

func handleVerify(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != verifyRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != verifyRequestMagic ||
		request[4] != wireVersion || request[5] > verifyOpEscalate {
		return nil, bus.ModuleStatusInvalidRequest
	}
	op := request[5]
	first := int32(binary.LittleEndian.Uint32(request[8:12]))
	delegateRC := int32(binary.LittleEndian.Uint32(request[12:16]))
	maxSignalStatus := int32(binary.LittleEndian.Uint32(request[16:20]))
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	response := make([]byte, verifyResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], verifyResponseMagic)

	if op == verifyOpEscalate {
		outcome := VerifyOutcome(first)
		if outcome > VerifyInfraError {
			return nil, bus.ModuleStatusInvalidRequest
		}
		binary.LittleEndian.PutUint32(response[4:8], uint32(outcome))
		if VerifyEscalationWarranted(delegateRC, outcome) {
			binary.LittleEndian.PutUint32(response[8:12], 1)
		}
		return response, bus.ModuleStatusOK
	}

	// A ceiling below 128 cannot be a signal status; taking it would classify
	// every killed verifier as a work-product failure.
	if maxSignalStatus < 128 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	outcome := ClassifyVerify(first, maxSignalStatus)
	binary.LittleEndian.PutUint32(response[4:8], uint32(outcome))
	if VerifyEscalationWarranted(delegateRC, outcome) {
		binary.LittleEndian.PutUint32(response[8:12], 1)
	}
	return response, bus.ModuleStatusOK
}
