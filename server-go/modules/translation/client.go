// Package translation exposes the existing C translation owner to Go callers.
// The one-shot server mode has no transport, configuration, credential or daemon
// side effects; provider bytes exist only at this explicit codec boundary.
package translation

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"os/exec"
	"time"
	"unicode/utf8"
)

const frameMax = 8 << 20

var errCodec = errors.New("shared model translation unavailable")

// Executable is host-owned injection for callers using a relocated installed
// server (including tests). An empty value selects the installed server.
type Client struct{ Executable string }
type boundedBuffer struct{ bytes.Buffer }

func (b *boundedBuffer) Write(p []byte) (int, error) {
	if len(p) > frameMax-b.Len() {
		return 0, errCodec
	}
	return b.Buffer.Write(p)
}
func (c Client) call(ctx context.Context, request any) ([]byte, error) {
	body, err := json.Marshal(request)
	if err != nil || len(body) > frameMax {
		return nil, errCodec
	}
	defer clear(body)
	executable := c.Executable
	if executable == "" {
		executable = "/usr/local/bin/aimee-server"
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, executable, "--module-model-codec")
	command.Env = []string{"PATH=/usr/local/bin:/usr/bin:/bin"}
	command.Stdin = bytes.NewReader(body)
	var output boundedBuffer
	command.Stdout = &output
	if command.Run() != nil {
		clear(output.Bytes())
		return nil, errCodec
	}
	return output.Bytes(), nil
}
func (c Client) RenderRequest(ctx context.Context, request *canonical.Request, wire, profile string, disableLocalThinking bool) ([]byte, error) {
	raw, err := c.call(ctx, map[string]any{"operation": "render", "wire": wire, "profile": profile, "request": request, "disable_local_thinking": disableLocalThinking})
	if err != nil {
		return nil, err
	}
	defer clear(raw)
	var reply struct {
		Status string `json:"status"`
		Body   string `json:"body"`
	}
	if !canonical.Decode(raw, &reply) || reply.Status != "ok" || reply.Body == "" {
		return nil, errCodec
	}
	return []byte(reply.Body), nil
}
func (c Client) ParseResponse(ctx context.Context, body []byte, wire string) (*canonical.Response, error) {
	if !utf8.Valid(body) || bytes.IndexByte(body, 0) >= 0 {
		return nil, errCodec
	}
	raw, err := c.call(ctx, map[string]any{"operation": "parse", "wire": wire, "profile": "api", "body": string(body)})
	if err != nil {
		return nil, err
	}
	defer clear(raw)
	var reply struct {
		Status   string          `json:"status"`
		Response json.RawMessage `json:"response"`
	}
	if !canonical.Decode(raw, &reply) || reply.Status != "ok" {
		return nil, errCodec
	}
	return canonical.DecodeResponse(reply.Response)
}
