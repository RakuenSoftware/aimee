package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/JBailes/aimee/server-go/thinclient"
	"net/http"
	"path/filepath"
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

// The browser transport does not interpret conversation events. Native adapters
// have already produced and persisted canonical IR before these bytes arrive.
func chatIRHTTP(ctx context.Context, socketPath, username string, message json.RawMessage, sid, cwd, attachID string, cb func([]byte) error) error {
	payload, err := json.Marshal(map[string]any{"message": message, "aimee_session_id": sid, "cwd": cwd, "attach_id": attachID, "ir_version": 1, "queue": true, "queue_wait_ms": 30000})
	if err != nil {
		return err
	}
	sock := filepath.Join(filepath.Dir(socketPath), "aimee-http.sock")
	transport := &thinclient.Transport{Endpoint: "unix:" + sock}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "http://aimee/v1/chat/stream", bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Aimee-Webuser", username)
	response, err := (&http.Client{Transport: transport}).Do(req)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("chat stream HTTP %d", response.StatusCode)
	}
	scanner := bufio.NewScanner(response.Body)
	scanner.Buffer(make([]byte, 65536), 4*1024*1024)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(bytes.TrimSpace(line)) != 0 {
			if err = cb(line); err != nil {
				return err
			}
		}
	}
	return scanner.Err()
}
