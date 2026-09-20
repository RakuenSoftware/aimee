package economizer

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"io"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	requestBudgetMagic     uint32 = 0x54474442 // BDGT
	requestBudgetHeader           = 52
	requestBudgetLimitsMax        = 1024
)

// Admission is independent of optional cost-saving transforms. A declared hard
// limit cannot fail open when the reducer is off or its process is unavailable.
const (
	RequestBudgetAdmitted uint16 = iota
	RequestBudgetInvalid
	RequestBudgetOverflow
	RequestBudgetTokensUnavailable
)

type requestBudgetLimits struct {
	SchemaVersion          int     `json:"schema_version"`
	MaxRequestBytes        *uint64 `json:"max_request_bytes,omitempty"`
	MaxRequestTokens       *uint64 `json:"max_request_tokens,omitempty"`
	ReservedResponseTokens *uint64 `json:"reserved_response_tokens,omitempty"`
	ReservedToolTokens     *uint64 `json:"reserved_tool_tokens,omitempty"`
}

func admitRequestBudget(raw []byte, requestBytes uint64) uint16 {
	// Compact validates UTF-8, duplicate keys and escapes before encoding/json's
	// decoder can normalize them. Null limits are not absence or literal zero.
	compact, result := JSONCompact(raw)
	if result == JSONNotShorter {
		compact = raw
	} else if result != JSONOK {
		return RequestBudgetInvalid
	}
	var fields map[string]json.RawMessage
	if json.Unmarshal(compact, &fields) != nil || fields == nil {
		return RequestBudgetInvalid
	}
	for key, value := range fields {
		switch key {
		case "schema_version", "max_request_bytes", "max_request_tokens", "reserved_response_tokens", "reserved_tool_tokens":
		default:
			return RequestBudgetInvalid
		}
		if bytes.Equal(value, []byte("null")) {
			return RequestBudgetInvalid
		}
	}
	var limits requestBudgetLimits
	decoder := json.NewDecoder(bytes.NewReader(compact))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&limits) != nil || limits.SchemaVersion != 1 {
		return RequestBudgetInvalid
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return RequestBudgetInvalid
	}
	if limits.MaxRequestTokens != nil || limits.ReservedResponseTokens != nil || limits.ReservedToolTokens != nil {
		// No caller-supplied count is a tokenizer attestation. Never turn bytes/4 into
		// claimed hard-token compliance. A provider-bound counter must supply this.
		return RequestBudgetTokensUnavailable
	}
	if limits.MaxRequestBytes == nil {
		return RequestBudgetInvalid
	}
	if requestBytes > *limits.MaxRequestBytes {
		return RequestBudgetOverflow
	}
	return RequestBudgetAdmitted
}

// The authenticated host supplies the length and SHA-256 of its immutable final
// provider body. Only this bounded metadata crosses the bus: prompt content is
// neither duplicated nor captured by the admission call. The response commitment
// binds route, body digest, byte length and the exact requested limits together.
func handleRequestBudget(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < requestBudgetHeader || len(request) > requestBudgetHeader+requestBudgetLimitsMax ||
		binary.LittleEndian.Uint32(request[:4]) != requestBudgetMagic ||
		binary.LittleEndian.Uint16(request[4:6]) != auxWireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	route := binary.LittleEndian.Uint16(request[6:8])
	length := binary.LittleEndian.Uint32(request[48:52])
	if route < 1 || route > 3 || int(length) != len(request)-requestBudgetHeader || length == 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	result := admitRequestBudget(request[requestBudgetHeader:], binary.LittleEndian.Uint64(request[8:16]))
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	commitment := sha256.Sum256(request)
	return auxResponse(requestBudgetMagic, result, commitment[:]), bus.ModuleStatusOK
}
