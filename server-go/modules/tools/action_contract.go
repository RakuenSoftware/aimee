package tools

import (
	"bytes"
	"encoding/json"
	"io"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const EventActionResource uint32 = 6914
const StageActionResource uint32 = 2

// The resource resolver runs at the tool owner. Its result is transient: raw
// effective arguments are carried to dispatch and never stored in the journal.
func handleActionResource(invocation ring.ModuleInvocation, raw []byte) ([]byte, ring.ModuleStatus) {
	if invocation.PrincipalRef != 0 || len(raw) > 65536 {
		return nil, ring.ModuleStatusInvalidRequest
	}
	verb, args, caller, err := ring.DecodeCommandWithContext(raw)
	if err != nil || (verb != "describe" && verb != "verify") || caller == nil || !caller.Authenticated {
		return nil, ring.ModuleStatusInvalidRequest
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
		return nil, ring.ModuleStatusInvalidRequest
	}
	var resource any
	if verb == "verify" {
		resource, err = VerifyGovernedAction(req.Tool, req.Arguments, req.Directory, req.Destination, req.PayloadDigest)
	} else {
		resource, err = DescribeGovernedAction(req.Tool, req.Arguments, req.Directory)
	}
	if err != nil {
		return nil, ring.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(resource)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	framed, err := ring.EncodeCommandResult(body)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	return framed, ring.ModuleStatusOK
}
