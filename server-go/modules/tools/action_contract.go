package tools

import (
	"bytes"
	"encoding/json"
	"io"

	"github.com/JBailes/aimee/server-go/bus"
)

const EventActionResource uint32 = 6914
const StageActionResource uint32 = 2

// The resource resolver runs at the tool owner. Its result is transient: raw
// effective arguments are carried to dispatch and never stored in the journal.
func handleActionResource(invocation bus.ModuleInvocation, raw []byte) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || len(raw) > 65536 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	verb, args, caller, err := bus.DecodeCommandWithContext(raw)
	if err != nil || (verb != "describe" && verb != "verify") || caller == nil || !caller.Authenticated {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var req struct {
		Tool          string          `json:"tool"`
		Arguments     json.RawMessage `json:"arguments"`
		Directory     string          `json:"directory"`
		Destination   string          `json:"destination,omitempty"`
		PayloadDigest string          `json:"payload_sha256,omitempty"`
	}
	decoder := json.NewDecoder(bytes.NewReader(args))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&req) != nil || decoder.Decode(new(any)) != io.EOF {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var resource any
	if verb == "verify" {
		resource, err = VerifyGovernedAction(req.Tool, req.Arguments, req.Directory, req.Destination, req.PayloadDigest)
	} else {
		resource, err = DescribeGovernedAction(req.Tool, req.Arguments, req.Directory)
	}
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(resource)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	framed, err := bus.EncodeCommandResult(body)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return framed, bus.ModuleStatusOK
}
