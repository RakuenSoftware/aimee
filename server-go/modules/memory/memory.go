// Package memory implements the memory process wire contract.
package memory

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventExtractIndex uint32 = 5889
	EventWrite        uint32 = 5890
	EventEmbed        uint32 = 5891
	EventRetrieve     uint32 = 5892
	EventRerank       uint32 = 5893

	StageExtractIndex uint32 = 1
	StageWrite        uint32 = 2
	StageEmbed        uint32 = 3
	StageRetrieve     uint32 = 4
	StageRerank       uint32 = 5

	requestMagic  uint32 = 0x4b4e524d
	responseMagic uint32 = 0x464e434d
	wireVersion   uint32 = 1
	requestLen           = 16
	responseLen          = 8

	gateRequestMagic  uint32 = 0x54524757
	gateResponseMagic uint32 = 0x56524757
	relTypeMax               = 256
	gateRequestLen           = 20 + relTypeMax
	gateResponseLen          = 8

	extractRequestMagic      uint32 = 0x51525458
	extractResponseMagic     uint32 = 0x53525458
	extractRequestHeaderLen         = 16
	extractResponseHeaderLen        = 8
	// Field capacities of one triple, mirroring pattern_triple_t's buffers. A
	// field is never emitted longer than these -- ExtractPatterns already trims
	// to them -- but the C decoder refuses an over-long field outright, so
	// emitting one would be a hard failure rather than a truncation.
	tripleSubjectMax = 128
	tripleRelTypeMax = 64
	tripleObjectMax  = 128

	piiRequestMagic     uint32 = 0x51524950
	piiResponseMagic    uint32 = 0x53524950
	piiRequestHeaderLen        = 12
	piiResponseLen             = 8
)

const (
	ConfidenceLow uint32 = iota + 1
	ConfidenceMedium
	ConfidenceHigh
)

// Handle dispatches a memory stage call.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	switch invocation.StageID {
	case StageWrite:
		return handleWrite(invocation, request)
	case StageExtractIndex:
		return handleExtract(invocation, request)
	case StageRetrieve:
		return handlePIITurn(invocation, request)
	}
	return handleRerank(invocation, request)
}

// handleExtract runs the pattern-first extraction over a turn's text.
//
// The text is length-prefixed and unbounded on the wire: unlike a relation
// label, there is no length past which a turn stops being a turn. The bus body
// cap is the only limit, and it is enforced before the request gets here.
func handleExtract(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < extractRequestHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != extractRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	max := binary.LittleEndian.Uint32(request[8:12])
	textLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if max == 0 || textLen != len(request)-extractRequestHeaderLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	triples := ExtractPatterns(string(request[extractRequestHeaderLen:]), int(max))
	response := make([]byte, extractResponseHeaderLen)
	binary.LittleEndian.PutUint32(response[0:4], extractResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(triples)))
	for _, triple := range triples {
		if len(triple.Subject) >= tripleSubjectMax || len(triple.RelType) >= tripleRelTypeMax ||
			len(triple.Object) >= tripleObjectMax {
			// Unreachable through ExtractPatterns, which trims to these bounds.
			// Refuse rather than emit a field the C decoder will reject anyway,
			// so the failure names the stage instead of the transport.
			return nil, bus.ModuleStatusInvalidRequest
		}
		var kinds [8]byte
		binary.LittleEndian.PutUint32(kinds[0:4], uint32(triple.SubjectKind))
		binary.LittleEndian.PutUint32(kinds[4:8], uint32(triple.ObjectKind))
		response = append(response, kinds[:]...)
		for _, field := range []string{triple.Subject, triple.RelType, triple.Object} {
			var length [4]byte
			binary.LittleEndian.PutUint32(length[:], uint32(len(field)))
			response = append(response, length[:]...)
			response = append(response, field...)
		}
	}
	return response, bus.ModuleStatusOK
}

// handleWrite validates a candidate typed fact against the seed ontology.
//
// A relation the wire cannot carry never reaches here: the encoder refuses a
// label over relTypeMax rather than truncating it, so a length past the bound is
// a malformed request, not a long fact.
func handleWrite(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != gateRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != gateRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint16(request[18:20]) != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	length := int(binary.LittleEndian.Uint16(request[16:18]))
	if length > relTypeMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	headKind := NodeKind(binary.LittleEndian.Uint32(request[8:12]))
	tailKind := NodeKind(binary.LittleEndian.Uint32(request[12:16]))
	verdict := GateCheck(headKind, string(request[20:20+length]), tailKind)
	response := make([]byte, gateResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], gateResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(verdict))
	return response, bus.ModuleStatusOK
}

// handlePIITurn answers whether a turn explicitly asks for sensitive
// information -- the once-per-turn half of the PII recall gate.
func handlePIITurn(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < piiRequestHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != piiRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		int(binary.LittleEndian.Uint32(request[8:12])) != len(request)-piiRequestHeaderLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	answer := uint32(0)
	if TurnRequestsSensitive(string(request[piiRequestHeaderLen:])) {
		answer = 1
	}
	response := make([]byte, piiResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], piiResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], answer)
	return response, bus.ModuleStatusOK
}

// handleRerank classifies a fixed-point reranking score into a confidence band.
func handleRerank(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageRerank || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	score := int64(binary.LittleEndian.Uint64(request[8:16]))
	confidence := uint32(ConfidenceLow)
	if score >= 660000 {
		confidence = ConfidenceHigh
	} else if score >= 330000 {
		confidence = ConfidenceMedium
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], confidence)
	return response, bus.ModuleStatusOK
}
