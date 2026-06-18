package main

// git.go: webchat git-projects proxies (webchat-git WP-F). The browser never
// holds git credentials or absolute server paths: these handlers forward to
// aimee-server's first-class /v1/workspace/* routes over the token-bearing,
// X-Aimee-Webuser-asserted channel (v1RequestWebuser, see vault.go). Credentials
// live only in the user's sealed server vault; webchat just relays the action.

import (
	"context"
	"encoding/json"
	"net/http"
)

// gitRelay writes a sanitized browser response from an aimee-server reply,
// passing through a small allowlist of fields ({ok, name, output, projects})
// and never the raw upstream body.
func (s *server) gitRelay(w http.ResponseWriter, st int, data []byte, err error) {
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "git: aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, st, vaultSafeErrorMessage(data))
		return
	}
	var up struct {
		OK       bool     `json:"ok"`
		Name     string   `json:"name"`
		Output   string   `json:"output"`
		Projects []string `json:"projects"`
		Hosts    []string `json:"hosts"`
	}
	_ = json.Unmarshal(data, &up)
	out, _ := json.Marshal(map[string]any{
		"ok": true, "name": up.Name, "output": up.Output, "projects": up.Projects, "hosts": up.Hosts,
	})
	w.Write(out)
}

// /api/git/credentials — manage aimee-server's per-host git access tokens
// (GET list hosts, POST {host, token} set, DELETE {host} remove). Tokens are
// write-only: listing returns host names only, never secrets.
func (s *server) handleGitCredentials(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	user := currentUser(r)
	switch r.Method {
	case http.MethodGet:
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodGet, "/v1/git/credentials", nil)
		s.gitRelay(w, st, data, err)
	case http.MethodPost, http.MethodDelete:
		var req struct {
			Host  string `json:"host"`
			Token string `json:"token"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Host == "" {
			writeJSONError(w, http.StatusBadRequest, "host required")
			return
		}
		body, _ := json.Marshal(map[string]string{"host": req.Host, "token": req.Token})
		st, data, err := s.v1RequestWebuser(ctx, user, r.Method, "/v1/git/credentials", body)
		s.gitRelay(w, st, data, err)
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

// GET /api/git/projects — list the user's cloned projects.
func (s *server) handleGitProjects(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodGet, "/v1/workspace/projects", nil)
	s.gitRelay(w, st, data, err)
}

// POST /api/git/clone {url, name?, token?} — clone a repo as a project. An
// optional token authenticates a private repo and is persisted server-side
// (per host); it is never echoed back to the browser.
func (s *server) handleGitClone(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		URL   string `json:"url"`
		Name  string `json:"name"`
		Token string `json:"token"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.URL == "" {
		writeJSONError(w, http.StatusBadRequest, "url required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	body, _ := json.Marshal(map[string]string{"url": req.URL, "name": req.Name, "token": req.Token})
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/workspace/clone", body)
	s.gitRelay(w, st, data, err)
}

// POST /api/git/op {project, op, message?, branch?, n?} — run a git operation.
func (s *server) handleGitOp(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Project string `json:"project"`
		Op      string `json:"op"`
		Message string `json:"message"`
		Branch  string `json:"branch"`
		N       int    `json:"n"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Project == "" || req.Op == "" {
		writeJSONError(w, http.StatusBadRequest, "project and op required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	body, _ := json.Marshal(map[string]any{
		"project": req.Project, "op": req.Op, "message": req.Message, "branch": req.Branch, "n": req.N,
	})
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/workspace/git", body)
	s.gitRelay(w, st, data, err)
}
