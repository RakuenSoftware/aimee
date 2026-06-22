package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/RakuenSoftware/smoothgui/auth"
)

// pathWithin reports whether child is parent or a descendant of it (both cleaned).
func pathWithin(child, parent string) bool {
	child = filepath.Clean(child)
	parent = filepath.Clean(parent)
	return child == parent || strings.HasPrefix(child, parent+string(filepath.Separator))
}

// webuserWorkspaceRoot returns the caller's scoped workspace root (the path the
// agent's cwd must stay within), via aimee-server's /v1/workspace/projects.
func (s *server) webuserWorkspaceRoot(ctx context.Context, user string) (string, error) {
	st, data, err := s.v1RequestWebuser(ctx, user, http.MethodGet, "/v1/workspace/projects", nil)
	if err != nil {
		return "", err
	}
	if st != http.StatusOK {
		return "", fmt.Errorf("workspace lookup status %d", st)
	}
	var d struct {
		Root string `json:"root"`
	}
	if json.Unmarshal(data, &d) != nil {
		return "", fmt.Errorf("bad workspace response")
	}
	return d.Root, nil
}

type chatProject struct {
	Name      string `json:"name"`
	Root      string `json:"root"`
	ScannedAt string `json:"scanned_at,omitempty"`
	Current   bool   `json:"current,omitempty"`
}

// sseWrite sends one SSE frame: "event: <type>\ndata: <json>\n\n"
func sseWrite(w http.ResponseWriter, eventType string, data any) {
	b, err := json.Marshal(data)
	if err != nil {
		return
	}
	fmt.Fprintf(w, "event: %s\ndata: %s\n\n", eventType, b)
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
}

// handleChatSend streams a chat response from aimee-server as SSE.
func (s *server) handleChatSend(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		AimeeSessionID  string `json:"aimee_session_id"`
		Message         string `json:"message"`
		ClaudeSessionID string `json:"claude_session_id"`
		Cwd             string `json:"cwd"`
		AttachID        string `json:"attach_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"error":"bad request"}`, http.StatusBadRequest)
		return
	}
	if req.Message == "" {
		http.Error(w, `{"error":"message required"}`, http.StatusBadRequest)
		return
	}
	cwd, err := resolveProjectRoot(req.Cwd)
	if err != nil {
		http.Error(w, fmt.Sprintf(`{"error":%q}`, err.Error()), http.StatusBadRequest)
		return
	}
	// cwd containment: the agent runs server-side with file/exec tools, and cwd
	// comes from the browser — so it MUST stay within the caller's own scoped
	// workspace. aimee-server can't enforce this (the chat stream carries no
	// webuser identity), so webchat (the trust boundary) does it. Default to the
	// user's workspace when no project is selected; refuse anything outside it
	// (fail closed: a selected cwd we can't verify is also refused).
	if root, rerr := s.webuserWorkspaceRoot(r.Context(), currentUser(r)); rerr == nil && root != "" {
		if req.Cwd == "" {
			cwd = root
		} else if !pathWithin(cwd, root) {
			http.Error(w, `{"error":"project must be within your workspace"}`, http.StatusForbidden)
			return
		}
	} else if req.Cwd != "" {
		http.Error(w, `{"error":"could not verify your workspace"}`, http.StatusForbidden)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)

	doneSent := false
	// effectiveSID tracks the aimee conversation id for this turn: the one the
	// SPA supplied, or the one aimee-server mints (delivered as a "session"
	// event) for a brand-new tab. It is persisted against the logged-in user
	// after the turn so the tab can be restored later.
	effectiveSID := req.AimeeSessionID
	// Stream the chat over aimee-server's /v1 HTTP transport (native
	// POST /v1/chat/stream); the legacy NDJSON socket helper (chatStream)
	// remains available as a fallback.
	err = chatStreamHTTP(r.Context(), s.cfg.socketPath, req.Message, req.AimeeSessionID,
		req.ClaudeSessionID, cwd, req.AttachID, func(evt streamEvent) {
			switch evt.Event {
			case "turn_start":
				sseWrite(w, "turn_start", map[string]any{})
			case "text":
				sseWrite(w, "text", map[string]string{"content": evt.Content})
			case "thinking":
				sseWrite(w, "thinking", map[string]string{"content": evt.Content})
			case "turn_end":
				sseWrite(w, "turn_end", map[string]any{})
			case "session":
				if evt.ID != "" {
					effectiveSID = evt.ID
				}
				sseWrite(w, "session", map[string]string{"id": evt.ID})
			case "usage":
				kind := evt.Kind
				if kind == "" {
					kind = "realized"
				}
				sseWrite(w, "usage", map[string]any{
					"in":         evt.In,
					"out":        evt.Out,
					"cost":       evt.Cost,
					"usage_kind": kind,
				})
			case "done":
				doneSent = true
				sseWrite(w, "done", map[string]any{})
			case "error":
				msg := evt.Message
				if msg == "" {
					msg = evt.Error
				}
				if msg == "" {
					msg = "server error"
				}
				sseWrite(w, "error", map[string]string{"message": msg})
			}
		})

	if r.Context().Err() != nil {
		return
	}
	// Record this tab's session against the logged-in user so it survives a
	// browser/laptop crash and can be restored from /api/chat/threads. The
	// conversation itself already lives on aimee-server; this only persists the
	// ownership + lightweight metadata. Best-effort: a failure here must not
	// break the chat response that already streamed.
	if uerr := s.touchChatSession(currentUser(r), effectiveSID, cwd, req.Message); uerr != nil {
		log.Printf("aimee-webchat: persist session %q: %v", effectiveSID, uerr)
	}
	if err != nil {
		sseWrite(w, "error", map[string]string{"message": err.Error()})
	}
	if !doneSent {
		sseWrite(w, "done", map[string]any{})
	}
}

// handleChatProjects returns indexed projects plus the webchat process cwd fallback.
func (s *server) handleChatProjects(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	currentRoot, err := resolveProjectRoot("")
	if err != nil {
		currentRoot = "."
	}
	projects := []chatProject{{
		Name:    projectName(currentRoot),
		Root:    currentRoot,
		Current: true,
	}}
	seen := map[string]bool{currentRoot: true}

	if s.cfg != nil {
		if resp, err := s.socketCallForRequest(r, map[string]any{"method": "index.list"}); err == nil && resp != nil {
			var payload struct {
				Projects []chatProject `json:"projects"`
			}
			if raw, ok := resp["projects"]; ok && json.Unmarshal(raw, &payload.Projects) == nil {
				for _, project := range payload.Projects {
					root, err := resolveProjectRoot(project.Root)
					if err != nil || seen[root] {
						continue
					}
					project.Root = root
					if project.Name == "" {
						project.Name = projectName(root)
					}
					projects = append(projects, project)
					seen[root] = true
				}
			}
		}
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"current_root": currentRoot,
		"projects":     projects,
	})
}

func projectName(root string) string {
	name := filepath.Base(root)
	if name == "." || name == string(filepath.Separator) || name == "" {
		return root
	}
	return name
}

func resolveProjectRoot(raw string) (string, error) {
	root := strings.TrimSpace(raw)
	if root == "" {
		cwd, err := os.Getwd()
		if err != nil {
			return "", fmt.Errorf("failed to resolve working directory")
		}
		root = cwd
	}
	if !filepath.IsAbs(root) {
		return "", fmt.Errorf("project path must be absolute")
	}
	clean := filepath.Clean(root)
	st, err := os.Stat(clean)
	if err != nil {
		return "", fmt.Errorf("project path is not accessible")
	}
	if !st.IsDir() {
		return "", fmt.Errorf("project path is not a directory")
	}
	return clean, nil
}

// handleBootstrapStatus checks if .aimee-rules exists in the selected project.
func (s *server) handleBootstrapStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	cwd, err := resolveProjectRoot(r.URL.Query().Get("cwd"))
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}

	_, statErr := os.Stat(filepath.Join(cwd, ".aimee-rules"))
	hasRules := statErr == nil

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"has_rules": hasRules,
		"stacks":    []any{},
	})
}

const generatedRulesContent = `# Aimee Rules

- Follow the repository's existing architecture and conventions.
- Keep changes scoped to the requested behavior.
- Run the most relevant local checks before considering work complete.
`

// handleChatInitRules creates .aimee-rules for POST /api/chat/init-rules.
func (s *server) handleChatInitRules(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Cwd string `json:"cwd"`
	}
	_ = json.NewDecoder(r.Body).Decode(&req)

	cwd, err := resolveProjectRoot(req.Cwd)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	rulesPath := filepath.Join(cwd, ".aimee-rules")
	created := false

	file, err := os.OpenFile(rulesPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err == nil {
		if _, err = file.WriteString(generatedRulesContent); err != nil {
			_ = file.Close()
			writeJSONError(w, http.StatusInternalServerError, "failed to write .aimee-rules")
			return
		}
		if err = file.Close(); err != nil {
			writeJSONError(w, http.StatusInternalServerError, "failed to close .aimee-rules")
			return
		}
		created = true
	} else if !os.IsExist(err) {
		writeJSONError(w, http.StatusInternalServerError, "failed to create .aimee-rules")
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ok":             true,
		"status":         "ok",
		"created":        created,
		"already_exists": !created,
	})
}

// handleChatSkills returns an empty skills list.
func (s *server) handleChatSkills(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `[]`)
}

// handleChatSkill handles POST /api/chat/skill.
func (s *server) handleChatSkill(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"status":"ok"}`)
}

// handleChatPromptTier handles GET/POST /api/chat/prompt-tier.
func (s *server) handleChatPromptTier(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method == http.MethodGet {
		fmt.Fprintf(w, `{"tier":"standard"}`)
		return
	}
	fmt.Fprintf(w, `{"status":"ok","tier":"standard"}`)
}

// handleChatClear handles POST /api/chat/clear.
func (s *server) handleChatClear(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"status":"ok"}`)
}

// handleChatInterrupt handles POST /api/chat/interrupt: stop the in-flight turn
// for a session and queue a steering message the server auto-continues as the
// next turn (streamed to the session's /events). The reply is small JSON
// ({interrupted:bool}); the steered turn itself arrives on the events stream.
func (s *server) handleChatInterrupt(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		AimeeSessionID string `json:"aimee_session_id"`
		Message        string `json:"message"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"error":"bad request"}`, http.StatusBadRequest)
		return
	}
	if req.AimeeSessionID == "" || req.Message == "" {
		http.Error(w, `{"error":"aimee_session_id and message required"}`, http.StatusBadRequest)
		return
	}
	body, _ := json.Marshal(map[string]string{
		"aimee_session_id": req.AimeeSessionID,
		"message":          req.Message,
	})
	st, data, err := s.v1RequestWebuser(r.Context(), currentUser(r), http.MethodPost,
		"/v1/chat/interrupt", body)
	if err != nil {
		http.Error(w, fmt.Sprintf(`{"error":%q}`, err.Error()), http.StatusBadGateway)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(st)
	w.Write(data)
}

// handleChatThreads is a stub for GET /api/chat/threads (in-tab conversation
// branching — a separate, not-yet-implemented feature). It returns the empty
// shape the SPA's ThreadBar expects. The per-user persisted tab list lives at
// GET /api/chat/sessions (handleChatSessionsList), not here.
func (s *server) handleChatThreads(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"threads":[],"active_id":0}`)
}

// handleChatBranch is a stub for POST /api/chat/branch.
func (s *server) handleChatBranch(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"status":"ok"}`)
}

// handleChatSwitchThread is a stub for POST /api/chat/switch-thread.
func (s *server) handleChatSwitchThread(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"status":"ok"}`)
}

// handleChatRewind is a stub for POST /api/chat/rewind.
func (s *server) handleChatRewind(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"status":"ok"}`)
}

// requireAuth validates the session cookie. For API paths it returns 401 JSON;
// for page paths it redirects to /login.
func (s *server) requireAuth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var username string
		cookie, err := r.Cookie("session")
		if err == nil {
			username, err = s.sessions.ValidateSession(cookie.Value)
		}
		if err != nil {
			if isAPIPath(r.URL.Path) {
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(http.StatusUnauthorized)
				fmt.Fprintf(w, `{"error":"authentication required"}`)
				return
			}
			http.Redirect(w, r, "/login", http.StatusFound)
			return
		}
		// Carry the validated username so handlers can scope persistence
		// (chat sessions) to the logged-in user.
		h(w, withUser(r, username))
	}
}

// requireAuthMiddleware wraps an http.Handler.
func (s *server) requireAuthMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.requireAuth(next.ServeHTTP)(w, r)
	})
}

func isAPIPath(path string) bool {
	return len(path) >= 4 && path[:4] == "/api"
}

// handleMe returns the authenticated user's info.
func (s *server) handleMe(w http.ResponseWriter, r *http.Request) {
	username := auth.GetUsername(r)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"username": username})
}
