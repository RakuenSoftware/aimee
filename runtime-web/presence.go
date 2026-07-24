package main

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"path/filepath"
	"strconv"
)

// Unified-presence integration for webchat. A browser tab attaches a "webchat"
// surface to its aimee session (POST /v1/sessions/{id}/attach), forwards the
// minted attach_id on each chat turn (so aimee-server serializes turns across
// surfaces), optionally subscribes to the session's presence-event stream, and
// detaches on close. These handlers proxy to aimee-server's /v1 surface over
// the HTTP-over-UDS socket, following the persona.go pattern.

// handleChatAttach proxies POST /api/chat/attach {sid} → POST
// /v1/sessions/{sid}/attach, registering a "webchat" surface and returning the
// minted attach_id (and events_url) to the browser.
func (s *server) handleChatAttach(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Sid string `json:"sid"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Sid == "" {
		writeJSONError(w, http.StatusBadRequest, "sid required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	// subscribe_mask 1 == PRESENCE_EV_TURN: the live turn stream. Not persistent
	// — a browser tab is a live surface, not a durable delivery target.
	body, _ := json.Marshal(map[string]any{"surface": "webchat", "subscribe_mask": 1})
	st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/sessions/"+req.Sid+"/attach", body)
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, http.StatusBadGateway, "attach refused")
		return
	}
	w.Write(data)
}

// handleChatDetach proxies POST /api/chat/detach {sid, attach_id} → POST
// /v1/sessions/{sid}/detach. Fire-and-forget from the browser's perspective.
func (s *server) handleChatDetach(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Sid      string `json:"sid"`
		AttachID string `json:"attach_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Sid == "" || req.AttachID == "" {
		writeJSONError(w, http.StatusBadRequest, "sid and attach_id required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	body, _ := json.Marshal(map[string]string{"attach_id": req.AttachID})
	st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/sessions/"+req.Sid+"/detach", body)
	if err != nil || st != http.StatusOK {
		// Best-effort: a failed detach is non-fatal (the presence is torn down
		// when the session closes anyway).
		w.Write([]byte(`{"detached":false}`))
		return
	}
	w.Write(data)
}

// handleChatSessionEvents proxies GET /api/chat/session-events?sid=<id> to the
// long-lived SSE stream GET /v1/sessions/{sid}/events, relaying presence events
// (turn_started / turn_delta / turn_done and routed async events) to the
// browser's EventSource so it can show when aimee is active on another surface.
// Streams raw bytes through with a flush per chunk; bounded by the request
// context (closed when the browser disconnects).
func (s *server) handleChatSessionEvents(w http.ResponseWriter, r *http.Request) {
	sid := r.URL.Query().Get("sid")
	if sid == "" {
		writeJSONError(w, http.StatusBadRequest, "sid required")
		return
	}
	sock := filepath.Join(filepath.Dir(s.cfg.socketPath), "aimee-http.sock")
	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return (&net.Dialer{}).DialContext(ctx, "unix", sock)
			},
		},
	}
	// Forward a resume cursor so the browser resumes after a reconnect/remount
	// instead of replaying the whole ring from 0. Validated as a uint to keep it
	// out of the upstream URL if malformed.
	upstream := "http://aimee/v1/sessions/" + sid + "/events"
	if cur := r.URL.Query().Get("cursor"); cur != "" {
		if _, perr := strconv.ParseUint(cur, 10, 64); perr == nil {
			upstream += "?cursor=" + cur
		}
	}
	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, upstream, nil)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "request build failed")
		return
	}
	// Forward the EventSource auto-reconnect cursor so the upstream resumes from
	// the last delivered event instead of replaying the ring from 0.
	if leid := r.Header.Get("Last-Event-ID"); leid != "" {
		req.Header.Set("Last-Event-ID", leid)
	}
	resp, err := client.Do(req)
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "aimee-server unavailable")
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		writeJSONError(w, http.StatusBadGateway, "no such session")
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	flusher, _ := w.(http.Flusher)

	buf := make([]byte, 4096)
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := w.Write(buf[:n]); werr != nil {
				return
			}
			if flusher != nil {
				flusher.Flush()
			}
		}
		if rerr != nil {
			return
		}
	}
}
