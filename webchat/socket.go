package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const socketCallTimeout = 10 * time.Second
const chatReconnectTimeout = 30 * time.Second
const chatReconnectStep = 250 * time.Millisecond

// socketConn is a single newline-delimited-JSON connection to aimee-server.
type socketConn struct {
	conn net.Conn
	rd   *bufio.Reader
}

func dialAimee(ctx context.Context, socketPath string) (*socketConn, error) {
	d := net.Dialer{}
	c, err := d.DialContext(ctx, "unix", socketPath)
	if err != nil {
		return nil, fmt.Errorf("connect %s: %w", socketPath, err)
	}
	return &socketConn{conn: c, rd: bufio.NewReaderSize(c, 256*1024)}, nil
}

func dialAimeeRecovering(ctx context.Context, socketPath string) (*socketConn, error) {
	deadline := time.Now().Add(chatReconnectTimeout)
	backoff := chatReconnectStep
	var lastErr error
	for {
		sc, err := dialAimee(ctx, socketPath)
		if err == nil {
			if authErr := sc.authenticate(socketPath); authErr == nil {
				return sc, nil
			} else {
				lastErr = fmt.Errorf("auth: %w", authErr)
				sc.close()
			}
		} else {
			lastErr = err
		}
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		if time.Now().After(deadline) {
			if lastErr == nil {
				lastErr = fmt.Errorf("aimee-server unavailable")
			}
			return nil, lastErr
		}
		timer := time.NewTimer(backoff)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil, ctx.Err()
		case <-timer.C:
		}
		if backoff < time.Second {
			backoff *= 2
		}
	}
}

func (s *socketConn) send(v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	b = append(b, '\n')
	_, err = s.conn.Write(b)
	return err
}

func (s *socketConn) recv() (map[string]json.RawMessage, error) {
	line, err := s.rd.ReadString('\n')
	if err != nil {
		if err != io.EOF || strings.TrimSpace(line) == "" {
			return nil, err
		}
	}
	line = strings.TrimSpace(line)
	var msg map[string]json.RawMessage
	if err := json.Unmarshal([]byte(line), &msg); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}
	return msg, nil
}

func (s *socketConn) close() {
	s.conn.Close()
}

func (s *socketConn) closeOnContext(ctx context.Context) func() {
	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			s.close()
		case <-done:
		}
	}()
	return func() { close(done) }
}

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
	return fmt.Errorf("server: %s", errMsg)
}

// socketCall sends a single-shot RPC and returns the parsed response.
func socketCall(socketPath string, req map[string]any) (map[string]json.RawMessage, error) {
	ctx, cancel := context.WithTimeout(context.Background(), socketCallTimeout)
	defer cancel()
	return socketCallContext(ctx, socketPath, req)
}

func socketCallContext(ctx context.Context, socketPath string, req map[string]any) (map[string]json.RawMessage, error) {
	sc, err := dialAimee(ctx, socketPath)
	if err != nil {
		return nil, err
	}
	defer sc.close()
	stop := sc.closeOnContext(ctx)
	defer stop()

	if deadline, ok := ctx.Deadline(); ok {
		_ = sc.conn.SetDeadline(deadline)
	}

	if err := sc.authenticate(socketPath); err != nil {
		return nil, fmt.Errorf("auth: %w", err)
	}
	if err := sc.send(req); err != nil {
		return nil, fmt.Errorf("send: %w", err)
	}
	msg, err := sc.recv()
	if err != nil {
		return nil, fmt.Errorf("recv: %w", err)
	}
	if err := rpcError(msg); err != nil {
		return nil, err
	}
	return msg, nil
}

// authenticate reads server.token from ~/.config/aimee/ and sends auth.
func (s *socketConn) authenticate(socketPath string) error {
	dir := filepath.Dir(socketPath)
	tokenPath := filepath.Join(dir, "server.token")
	raw, err := os.ReadFile(tokenPath)
	if err != nil {
		return fmt.Errorf("read token %s: %w", tokenPath, err)
	}
	token := strings.TrimSpace(string(raw))

	if err := s.send(map[string]string{"method": "auth", "token": token}); err != nil {
		return err
	}
	resp, err := s.recv()
	if err != nil {
		return fmt.Errorf("auth response: %w", err)
	}
	var status string
	if err := json.Unmarshal(resp["status"], &status); err != nil || status != "ok" {
		return fmt.Errorf("auth rejected")
	}
	return nil
}

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

// chatStream sends chat.send_stream and calls cb for each intermediate event.
// Returns the final status or error.
func chatStream(ctx context.Context, socketPath, message, aimeeSessionID, claudeSessionID, cwd string, cb func(streamEvent)) error {
	sc, err := dialAimeeRecovering(ctx, socketPath)
	if err != nil {
		return err
	}
	defer sc.close()
	stop := sc.closeOnContext(ctx)
	defer stop()

	req := map[string]string{
		"method":            "chat.send_stream",
		"message":           message,
		"claude_session_id": claudeSessionID,
	}
	if aimeeSessionID != "" {
		req["aimee_session_id"] = aimeeSessionID
	}
	if cwd != "" {
		req["cwd"] = cwd
	}
	if err := sc.send(req); err != nil {
		return fmt.Errorf("send: %w", err)
	}

	for {
		msg, err := sc.recv()
		if err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return fmt.Errorf("recv: %w", err)
		}

		// Final message has "status" field.
		if _, ok := msg["status"]; ok {
			if err := rpcError(msg); err != nil {
				return err
			}
			return nil
		}

		// Intermediate event.
		var evt streamEvent
		b, _ := json.Marshal(msg)
		json.Unmarshal(b, &evt)
		cb(evt)
	}
}

// chatStreamHTTP is chatStream over the /v1 HTTP transport: it POSTs to
// aimee-server's native POST /v1/chat/stream over the Unix socket and reads the
// newline-delimited JSON event stream (application/x-ndjson), calling cb for each
// intermediate event. Same event shape as the NDJSON socket path, so callers are
// unchanged. The aimee-http.sock lives alongside the legacy RPC socket.
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
