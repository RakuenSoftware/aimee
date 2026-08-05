package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"path/filepath"
	"strings"
	"time"
)

const socketCallTimeout = 10 * time.Second

// cloneTimeout bounds a single full git clone; cloneOrgTimeout bounds a bulk
// org clone (up to 100 repos) — both far longer than a normal socket call.
const cloneTimeout = 2 * time.Minute
const cloneOrgTimeout = 5 * time.Minute

func rpcError(msg map[string]json.RawMessage) error {
	var status string
	if raw, ok := msg["status"]; ok {
		_ = json.Unmarshal(raw, &status)
	}
	if status == "" || status == "ok" {
		return nil
	}
	var errMsg string
	if raw, ok := msg["message"]; ok {
		_ = json.Unmarshal(raw, &errMsg)
	}
	if errMsg == "" {
		if raw, ok := msg["error"]; ok {
			_ = json.Unmarshal(raw, &errMsg)
		}
	}
	if errMsg == "" {
		errMsg = status
	}
	if httpStatus := rpcErrorHTTPStatus(msg); httpStatus != 0 {
		return &rpcFault{httpStatus: httpStatus, msg: errMsg}
	}
	return fmt.Errorf("server: %s", errMsg)
}

// rpcFault is a dispatch error the server classified through the runtime-web
// module. Plain errors stay plain, so callers that only print the message are
// unaffected.
type rpcFault struct {
	httpStatus int
	msg        string
}

func (e *rpcFault) Error() string { return "server: " + e.msg }

func rpcErrorHTTPStatus(msg map[string]json.RawMessage) int {
	raw, ok := msg["http_status"]
	if !ok {
		return 0
	}
	var status int
	if json.Unmarshal(raw, &status) != nil || status < 400 || status > 599 {
		return 0
	}
	return status
}

// rpcErrorStatus maps a dispatch error onto an HTTP status.
//
// Everything used to become 502 Bad Gateway, so `agent add` with no arguments
// answered "502: usage: agent add <name> <endpoint> <model>" — a usage message
// delivered as an upstream failure, which misleads whoever reads the logs and
// invites a client to retry a request that can never succeed.
//
// A response without a valid module-produced status still maps to 502. That is
// deliberate fail-closed transport behavior, not a local fault-classification
// fallback.
func rpcErrorStatus(err error) int {
	var fault *rpcFault
	if errors.As(err, &fault) {
		return fault.httpStatus
	}
	return http.StatusBadGateway
}

// socketCall sends a single-shot RPC and returns the parsed response.
// streamEvent is one NDJSON line from a streaming aimee-server response.
type streamEvent struct {
	Event string `json:"event"`
	// text/thinking/session events
	Content string `json:"content,omitempty"`
	ID      string `json:"id,omitempty"`
	// status (final message)
	Status  string `json:"status,omitempty"`
	Error   string `json:"error,omitempty"`
	Message string `json:"message,omitempty"`
	// usage event
	In   int64   `json:"in,omitempty"`
	Out  int64   `json:"out,omitempty"`
	Cost float64 `json:"cost,omitempty"`
	// usage_kind: realized (provider-reported) vs estimated/avoided/partial. The
	// webchat chat path uses provider-reported usage, so it is realized unless the
	// server overrides it.
	Kind string `json:"kind,omitempty"`
}

// chatStreamHTTP POSTs to
// aimee-server's native POST /v1/chat/stream over the Unix socket and reads the
// newline-delimited JSON event stream (application/x-ndjson), calling cb for each
// intermediate event. The public config still calls its historical socket field
// `socketPath`; the HTTP UDS lives beside that path.
func chatStreamHTTP(ctx context.Context, socketPath, message, aimeeSessionID, claudeSessionID, cwd, attachID string, cb func(streamEvent)) error {
	sock := filepath.Join(filepath.Dir(socketPath), "aimee-http.sock")

	reqBody := map[string]string{"message": message}
	if aimeeSessionID != "" {
		reqBody["aimee_session_id"] = aimeeSessionID
	}
	if claudeSessionID != "" {
		reqBody["claude_session_id"] = claudeSessionID
	}
	if cwd != "" {
		reqBody["cwd"] = cwd
	}
	// Unified-presence: when the browser tab has attached a "webchat" surface,
	// forward its attach_id so aimee-server serializes turns across surfaces
	// (declining a racing submit with presence_busy).
	if attachID != "" {
		reqBody["attach_id"] = attachID
	}
	payload, err := json.Marshal(reqBody)
	if err != nil {
		return err
	}

	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return (&net.Dialer{}).DialContext(ctx, "unix", sock)
			},
		},
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "http://aimee/v1/chat/stream",
		bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("chat stream: status %d: %s", resp.StatusCode, strings.TrimSpace(string(b)))
	}

	scanner := bufio.NewScanner(resp.Body)
	scanner.Buffer(make([]byte, 0, 64*1024), 4*1024*1024) // tolerate long event lines
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(bytes.TrimSpace(line)) == 0 {
			continue
		}
		var msg map[string]json.RawMessage
		if json.Unmarshal(line, &msg) != nil {
			continue
		}
		// The final message carries a "status" field (ok / error).
		if _, ok := msg["status"]; ok {
			return rpcError(msg)
		}
		var evt streamEvent
		json.Unmarshal(line, &evt)
		cb(evt)
	}
	if err := scanner.Err(); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return err
	}
	return nil
}
