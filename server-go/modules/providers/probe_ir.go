package providers

// Fixed probe policy uses the shared translation owner's pure pipe bridge.
import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/JBailes/aimee/server-go/modules/translation"
	"unicode/utf8"
)

const probePrompt = "Reply with exactly: ok"

// Existing C context ABI: aimee_context_{origin,authority,trust}_t.
// These are code-owned literals and untrusted diagnostic observations, never
// authenticated user/actor provenance or a credential capability.
const (
	probePlatform        = 1
	probeModel           = 7
	probeDiagnostic      = 0
	probeTaskInstruction = 2
	probePolicy          = 3
	probeUntrusted       = 1
	probeVerified        = 4
)

var errProbe = errors.New("provider returned no complete canonical diagnostic answer")

func probeRequest(model, wire string) *canonical.Request {
	prompt := probePrompt
	r := &canonical.Request{Version: 1, Model: model, SourceWire: wire, CachePolicy: "native",
		System: []canonical.Block{}, Messages: []canonical.Message{{Role: "user", ContentScalar: true, Blocks: []canonical.Block{{Type: "text", Text: &prompt, Context: &canonical.Context{Origin: probePlatform, Authority: probeTaskInstruction, Trust: probeVerified, ModelVisible: true}}}}},
		Tools: []canonical.Tool{}, Stops: []string{}, ToolChoice: canonical.ToolChoice{Mode: "unset"}}
	if wire == "responses" {
		policy := "You are performing a bounded availability probe."
		r.System = []canonical.Block{{Type: "text", Text: &policy, Context: &canonical.Context{Origin: probePlatform, Authority: probePolicy, Trust: probeVerified, ModelVisible: true}}}
		r.Stream = true
	} else if wire != "unspecified" {
		n := 256
		r.MaxTokens = &n
	}
	return r
}

var modelCodecExecutable = func() string { return "" }

func probeProfile(wire string) string {
	if wire == "responses" {
		return "codex"
	}
	return "api"
}
func probeWire(r *canonical.Request, localThinking bool) ([]byte, error) {
	return probeWireContext(context.Background(), r, localThinking)
}
func probeWireContext(ctx context.Context, r *canonical.Request, localThinking bool) ([]byte, error) {
	if r == nil || !r.Valid() {
		return nil, errProbe
	}
	owned, err := json.Marshal(r)
	fixed, _ := json.Marshal(probeRequest(r.Model, r.SourceWire))
	if err != nil || !bytes.Equal(owned, fixed) {
		return nil, errProbe
	}
	return (translation.Client{Executable: modelCodecExecutable()}).RenderRequest(ctx, r, r.SourceWire, probeProfile(r.SourceWire), localThinking && r.SourceWire == "openai_chat")
}

func probeResponseBase(raw []byte) *canonical.Response {
	role := "assistant"
	sidecar, _ := json.Marshal(object{"native_body_base64": base64.StdEncoding.EncodeToString(raw)})
	return &canonical.Response{Schema: "aimee.response.v1", Role: &role, Blocks: []canonical.ResponseBlock{}, Usage: canonical.ResponseUsage{Unobserved: true}, Raw: sidecar}
}
func probeHTTPResponse(body []byte, wire string) (*canonical.Response, error) {
	return probeHTTPResponseContext(context.Background(), body, wire)
}
func probeHTTPResponseContext(ctx context.Context, body []byte, wire string) (*canonical.Response, error) {
	diagnostic := probeResponseBase(body)
	if len(body) > 1<<20 || !utf8.Valid(body) || bytes.IndexByte(body, 0) >= 0 {
		return diagnostic, errProbe
	}
	response, err := (translation.Client{Executable: modelCodecExecutable()}).ParseResponse(ctx, body, wire)
	if err != nil {
		return diagnostic, err
	}

	// Retain exact external bytes beside the admitted native sidecar.
	sidecar, _ := json.Marshal(object{"native_body_base64": base64.StdEncoding.EncodeToString(body), "decoded_native": response.Raw})
	response.Raw = sidecar
	for i := range response.Blocks {
		response.Blocks[i].Context = &canonical.Context{Origin: probeModel, Authority: probeDiagnostic, Trust: probeUntrusted, ModelVisible: true}
	}
	if !response.Valid() {
		return diagnostic, errProbe
	}
	if !response.CompleteText() {
		return response, errProbe
	}
	return response, nil
}
