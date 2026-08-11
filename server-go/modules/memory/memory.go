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

	scanRequestMagic      uint32 = 0x51525452
	scanResponseMagic     uint32 = 0x53525452
	scanRequestHeaderLen         = 12
	scanResponseHeaderLen        = 16

	sensRequestMagic      uint32 = 0x51525350
	sensResponseMagic     uint32 = 0x53525350
	sensRequestHeaderLen         = 12
	sensResponseHeaderLen        = 8
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
		if len(request) >= 4 && binary.LittleEndian.Uint32(request[0:4]) == scanRequestMagic {
			return handleScanTurn(invocation, request)
		}
		return handleExtract(invocation, request)
	case StageRetrieve:
		return handleRetrieve(invocation, request)
	case StageEmbed:
		return handleEmbed(invocation, request)
	case StageDeclareCommands:
		return handleDeclareCommands(invocation, request)
	}
	return handleRerank(invocation, request)
}

// handleScanTurn answers both halves of the §4 correction pre-scan: whether the
// turn retracts something, and which attribute it names. One call, because the
// caller asks both about the same turn at the same moment.
func handleScanTurn(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < scanRequestHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != scanRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		int(binary.LittleEndian.Uint32(request[8:12])) != len(request)-scanRequestHeaderLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	text := string(request[scanRequestHeaderLen:])
	attr, hasAttr := PossessiveAttr(text)
	if len(attr) >= attrMax {
		// Unreachable: PossessiveAttr trims to attrMax. Refuse rather than emit a
		// field the C decoder rejects, so the failure names the stage.
		return nil, bus.ModuleStatusInvalidRequest
	}
	response := make([]byte, scanResponseHeaderLen, scanResponseHeaderLen+len(attr))
	binary.LittleEndian.PutUint32(response[0:4], scanResponseMagic)
	if IsRetraction(text) {
		binary.LittleEndian.PutUint32(response[4:8], 1)
	}
	if hasAttr {
		binary.LittleEndian.PutUint32(response[8:12], 1)
	}
	binary.LittleEndian.PutUint32(response[12:16], uint32(len(attr)))
	return append(response, attr...), bus.ModuleStatusOK
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

// handleRetrieve serves both halves of the PII recall gate. They share a stage
// and are told apart by their magic, so a request meant for one is rejected by
// the other rather than misparsed.
func handleRetrieve(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) >= 4 && binary.LittleEndian.Uint32(request[0:4]) == sensRequestMagic {
		return handleSensitivity(invocation, request)
	}
	return handlePIITurn(invocation, request)
}

// handleSensitivity classifies a whole recall block's relations at once.
func handleSensitivity(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < sensRequestHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != sensRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count := int(binary.LittleEndian.Uint32(request[8:12]))
	if count <= 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	response := make([]byte, sensResponseHeaderLen, sensResponseHeaderLen+count)
	binary.LittleEndian.PutUint32(response[0:4], sensResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(count))
	offset := sensRequestHeaderLen
	for i := 0; i < count; i++ {
		if offset+2 > len(request) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		length := int(binary.LittleEndian.Uint16(request[offset : offset+2]))
		offset += 2
		if length > relTypeMax || offset+length > len(request) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		response = append(response, byte(RelSensitivityOf(string(request[offset:offset+length]))))
		offset += length
	}
	// Trailing bytes mean the two sides disagree about the shape. Refuse rather
	// than answer for the names that happened to parse: a caller that gets fewer
	// tiers than it has facts cannot tell which fact each tier belongs to.
	if offset != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
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
