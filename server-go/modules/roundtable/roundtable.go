// Package roundtable implements the roundtable process wire contract.
package roundtable

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventDeliberate uint32 = 9473
	StageDeliberate uint32 = 1

	requestMagic        uint32 = 0x52475452
	responseMagic       uint32 = 0x44475452
	wireVersion         uint32 = 1
	severityMax                = 15
	requestLen                 = 40
	responseLen                = 32
	requestSeverityOff         = 24
	responseSeverityOff        = 16
)

const (
	ReplayMatch uint32 = iota
	ReplayCorrected
	ReplayContradicted
	ReplayNoEvidence
	ReplayVacuous
	ReplayIndexUnavailable
)

const (
	VerifyKeep uint32 = iota
	VerifyCap
	VerifyDegrade
	VerifyReject
)

func zeroPadding(value []byte) bool {
	for _, item := range value {
		if item != 0 {
			return false
		}
	}
	return true
}

func normalizedSeverity(claimed string) string {
	if claimed == "" {
		return "suggestion"
	}
	return claimed
}

func cappedSeverity(claimed string) string {
	if claimed == "blocking" {
		return "suggestion"
	}
	return normalizedSeverity(claimed)
}

// Handle serves the stages that need nothing but their arguments, so they are
// available whether or not this process can reach the delegate plane: the
// deliberation rubric and chunk planning. Review is added on top by NewHandler
// only when a reviewer exists.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID == StageChunkPlan {
		return handleChunkPlan(invocation, request)
	}
	if invocation.StageID != StageDeliberate || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[8:12]) > ReplayIndexUnavailable ||
		binary.LittleEndian.Uint32(request[12:16]) > 1 ||
		binary.LittleEndian.Uint32(request[16:20]) > severityMax ||
		binary.LittleEndian.Uint32(request[20:24]) != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	severityLen := int(binary.LittleEndian.Uint32(request[16:20]))
	if !zeroPadding(request[requestSeverityOff+severityLen:]) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	status := binary.LittleEndian.Uint32(request[8:12])
	factual := binary.LittleEndian.Uint32(request[12:16]) == 1
	claimed := string(request[requestSeverityOff : requestSeverityOff+severityLen])
	action := uint32(VerifyCap)
	severity := ""
	switch status {
	case ReplayContradicted, ReplayVacuous:
		action = VerifyReject
	case ReplayIndexUnavailable:
		action = VerifyDegrade
		severity = normalizedSeverity(claimed)
	case ReplayNoEvidence:
		severity = cappedSeverity(claimed)
	case ReplayMatch, ReplayCorrected:
		if factual {
			action = VerifyKeep
			severity = normalizedSeverity(claimed)
		} else {
			severity = cappedSeverity(claimed)
		}
	}

	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], action)
	binary.LittleEndian.PutUint32(response[8:12], uint32(len(severity)))
	copy(response[responseSeverityOff:], severity)
	return response, bus.ModuleStatusOK
}
