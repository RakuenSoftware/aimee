package economizer

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"io"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// RequestAdmissionPollInterval keeps the final provider gate responsive after
// idle periods in both the bundled and independently exported Go process.
const RequestAdmissionPollInterval = time.Millisecond

const (
	requestBudgetMagic        uint32 = 0x54474442 // BDGT
	requestBudgetHeader              = 52
	requestBudgetPolicyHeader        = 56
	requestBudgetLimitsMax           = 1024
)

// Admission is independent of optional cost-saving transforms. A declared hard
// limit cannot fail open when the reducer is off or its process is unavailable.
const (
	RequestBudgetAdmitted uint16 = iota
	RequestBudgetInvalid
	RequestBudgetOverflow
	RequestBudgetTokensUnavailable
)

// Four is the native client's transport-unavailable result, never an owner
// decision. Keep it reserved when extending the protocol.
const RequestBudgetPolicyInvalid uint16 = 5

type requestBudgetLimits struct {
	SchemaVersion          int     `json:"schema_version"`
	MaxRequestBytes        *uint64 `json:"max_request_bytes,omitempty"`
	MaxRequestTokens       *uint64 `json:"max_request_tokens,omitempty"`
	ReservedResponseTokens *uint64 `json:"reserved_response_tokens,omitempty"`
	ReservedToolTokens     *uint64 `json:"reserved_tool_tokens,omitempty"`
}

func decodeRequestBudget(raw []byte) (requestBudgetLimits, bool) {
	var limits requestBudgetLimits
	// Compact validates UTF-8, duplicate keys and escapes before encoding/json's
	// decoder can normalize them. Null limits are not absence or literal zero.
	compact, result := JSONCompact(raw)
	if result == JSONNotShorter {
		compact = raw
	} else if result != JSONOK {
		return limits, false
	}
	var fields map[string]json.RawMessage
	if json.Unmarshal(compact, &fields) != nil || fields == nil {
		return limits, false
	}
	for key, value := range fields {
		switch key {
		case "schema_version", "max_request_bytes", "max_request_tokens", "reserved_response_tokens", "reserved_tool_tokens":
		default:
			return limits, false
		}
		if bytes.Equal(value, []byte("null")) {
			return limits, false
		}
	}
	decoder := json.NewDecoder(bytes.NewReader(compact))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&limits) != nil || limits.SchemaVersion != 1 {
		return limits, false
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return limits, false
	}
	return limits, true
}

func (limits requestBudgetLimits) needsTokenCounter() bool {
	return limits.MaxRequestTokens != nil || limits.ReservedResponseTokens != nil || limits.ReservedToolTokens != nil
}

func admitRequestBudget(raw []byte, requestBytes uint64) uint16 {
	return admitRequestBudgetPolicy(raw, nil, requestBytes)
}

func admitRequestBudgetPolicy(raw, policy []byte, requestBytes uint64) uint16 {
	var ceiling *uint64
	if len(policy) != 0 {
		inherited, ok := decodeRequestBudget(policy)
		if !ok || inherited.MaxRequestBytes == nil || inherited.needsTokenCounter() {
			// Operator misconfiguration is not a malformed user request. Fail
			// closed until the deployment is corrected, without exposing values.
			return RequestBudgetPolicyInvalid
		}
		ceiling = inherited.MaxRequestBytes
	}
	var limits requestBudgetLimits
	if len(raw) != 0 {
		var ok bool
		limits, ok = decodeRequestBudget(raw)
		if !ok {
			return RequestBudgetInvalid
		}
	}
	if limits.needsTokenCounter() {
		// No caller-supplied count is a tokenizer attestation. Never turn bytes/4 into
		// claimed hard-token compliance. A provider-bound counter must supply this.
		return RequestBudgetTokensUnavailable
	}
	if limits.MaxRequestBytes != nil && (ceiling == nil || *limits.MaxRequestBytes < *ceiling) {
		ceiling = limits.MaxRequestBytes
	}
	if ceiling == nil {
		return RequestBudgetInvalid
	}
	if requestBytes > *ceiling {
		return RequestBudgetOverflow
	}
	return RequestBudgetAdmitted
}

// The authenticated host supplies the length and SHA-256 of its immutable final
// provider body. Only this bounded metadata crosses the bus: prompt content is
// neither duplicated nor captured by the admission call. The response commitment
// binds route, body digest, byte length and the exact requested limits together.
func handleRequestBudget(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < requestBudgetHeader || len(request) > requestBudgetPolicyHeader+2*requestBudgetLimitsMax ||
		binary.LittleEndian.Uint32(request[:4]) != requestBudgetMagic {
		return nil, bus.ModuleStatusInvalidRequest
	}
	route := binary.LittleEndian.Uint16(request[6:8])
	length := uint64(binary.LittleEndian.Uint32(request[48:52]))
	header, policyLength := requestBudgetHeader, uint64(0)
	switch binary.LittleEndian.Uint16(request[4:6]) {
	case auxWireVersion:
		if length == 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	case 2:
		if len(request) < requestBudgetPolicyHeader {
			return nil, bus.ModuleStatusInvalidRequest
		}
		header = requestBudgetPolicyHeader
		policyLength = uint64(binary.LittleEndian.Uint32(request[52:56]))
		if policyLength == 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	if route < 1 || route > 3 || length > requestBudgetLimitsMax || policyLength > requestBudgetLimitsMax || length+policyLength != uint64(len(request)-header) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	result := admitRequestBudgetPolicy(request[header:uint64(header)+length], request[uint64(header)+length:], binary.LittleEndian.Uint64(request[8:16]))
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	commitment := sha256.Sum256(request)
	return auxResponse(requestBudgetMagic, result, commitment[:]), bus.ModuleStatusOK
}
