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
	"net/url"
	"strings"
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
		Root     string   `json:"root"`
	}
	_ = json.Unmarshal(data, &up)
	out, _ := json.Marshal(map[string]any{
		"ok": true, "name": up.Name, "output": up.Output, "projects": up.Projects,
		"hosts": up.Hosts, "root": up.Root,
	})
	w.Write(out)
}

// gitOAuthRelay passes through the GitHub device-flow fields (no secrets — the
// device_code stays server-side; only the user-facing code/URI + status cross).
func (s *server) gitOAuthRelay(w http.ResponseWriter, st int, data []byte, err error) {
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
		OK              bool   `json:"ok"`
		UserCode        string `json:"user_code"`
		VerificationURI string `json:"verification_uri"`
		Interval        int    `json:"interval"`
		Status          string `json:"status"`
		Error           string `json:"error"`
		Configured      bool   `json:"configured"`
		ClientID        string `json:"client_id"`
	}
	_ = json.Unmarshal(data, &up)
	out, _ := json.Marshal(map[string]any{
		"ok": true, "user_code": up.UserCode, "verification_uri": up.VerificationURI,
		"interval": up.Interval, "status": up.Status, "error": up.Error,
		"configured": up.Configured, "client_id": up.ClientID,
	})
	w.Write(out)
}

// gitReposRelay passes through the org-repos enumeration ({provider, repos:[…]}).
// Typed passthrough (like gitOAuthRelay), never the raw upstream body.
func (s *server) gitReposRelay(w http.ResponseWriter, st int, data []byte, err error) {
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
		Provider string `json:"provider"`
		Repos    []struct {
			Name     string `json:"name"`
			CloneURL string `json:"clone_url"`
			SSHURL   string `json:"ssh_url"`
			Private  bool   `json:"private"`
		} `json:"repos"`
	}
	_ = json.Unmarshal(data, &up)
	out, _ := json.Marshal(map[string]any{"provider": up.Provider, "repos": up.Repos})
	w.Write(out)
}

// gitCloneOrgRelay passes through the bulk-clone per-repo results ({results:[…]}).
func (s *server) gitCloneOrgRelay(w http.ResponseWriter, st int, data []byte, err error) {
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
		Results []struct {
			Name    string  `json:"name"`
			OK      bool    `json:"ok"`
			Project *string `json:"project"`
			Error   *string `json:"error"`
		} `json:"results"`
	}
	_ = json.Unmarshal(data, &up)
	out, _ := json.Marshal(map[string]any{"results": up.Results})
	w.Write(out)
}

// /api/git/oauth/github/config — read (GET) or set (POST {client_id}) the GitHub
// OAuth App client ID so "Sign in with GitHub" can be configured from the UI.
func (s *server) handleGitOauthGithubConfig(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	user := currentUser(r)
	switch r.Method {
	case http.MethodGet:
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodGet, "/v1/git/oauth/github/config", nil)
		s.gitOAuthRelay(w, st, data, err)
	case http.MethodPost:
		var req struct {
			ClientID string `json:"client_id"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ClientID == "" {
			writeJSONError(w, http.StatusBadRequest, "client_id required")
			return
		}
		body, _ := json.Marshal(map[string]string{"client_id": req.ClientID})
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodPost, "/v1/git/oauth/github/config", body)
		s.gitOAuthRelay(w, st, data, err)
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

// POST /api/git/oauth/github/start — begin GitHub device-flow sign-in.
func (s *server) handleGitOauthGithubStart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost,
		"/v1/git/oauth/github/start", []byte(`{}`))
	s.gitOAuthRelay(w, st, data, err)
}

// POST /api/git/oauth/github/poll — poll device-flow completion.
func (s *server) handleGitOauthGithubPoll(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost,
		"/v1/git/oauth/github/poll", []byte(`{}`))
	s.gitOAuthRelay(w, st, data, err)
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
		// On revoke, aimee-server recycles the user's editor (its spawn env held
		// the now-deleted token). Evict our cached loopback port so the next
		// /vscode request re-ensures a freshly-spawned, credential-free editor
		// instead of dialing the dead port and self-healing only after a failure.
		if r.Method == http.MethodDelete && err == nil && st == http.StatusOK {
			editorPorts.Delete(user)
		}
		s.gitRelay(w, st, data, err)
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

// /api/git/sshkey — store (POST {ssh_key}) or remove (DELETE) the caller's SSH
// private key for git over SSH. The key is write-only: it goes to the user's
// encrypted vault server-side and is never read back to the browser. Storing
// needs the user's vault unlocked; aimee-server returns 423 if it is locked,
// which the UI surfaces as a prompt to unlock.
func (s *server) handleGitSSHKey(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	user := currentUser(r)
	switch r.Method {
	case http.MethodDelete:
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodDelete, "/v1/git/sshkey", nil)
		s.gitRelay(w, st, data, err)
	case http.MethodPost:
		// A private key is a few KB; cap the body so a huge POST can't force
		// large allocations + parse + re-marshal here and downstream.
		r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
		var req struct {
			SSHKey string `json:"ssh_key"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.SSHKey == "" {
			if _, ok := err.(*http.MaxBytesError); ok {
				writeJSONError(w, http.StatusRequestEntityTooLarge, "ssh key too large")
				return
			}
			writeJSONError(w, http.StatusBadRequest, "ssh_key required")
			return
		}
		body, err := json.Marshal(map[string]string{"ssh_key": req.SSHKey})
		if err != nil {
			writeJSONError(w, http.StatusInternalServerError, "could not encode request")
			return
		}
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodPost, "/v1/git/sshkey", body)
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
		Project   string `json:"project"`
		Op        string `json:"op"`
		Message   string `json:"message"`
		Branch    string `json:"branch"`
		N         int    `json:"n"`
		SessionID string `json:"session_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Project == "" || req.Op == "" {
		writeJSONError(w, http.StatusBadRequest, "project and op required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	// session_id (optional): run the op in the calling session's isolated worktree
	// — the same tree its agent edits — rather than the shared project checkout.
	body, _ := json.Marshal(map[string]any{
		"project": req.Project, "op": req.Op, "message": req.Message, "branch": req.Branch, "n": req.N,
		"session_id": req.SessionID,
	})
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/workspace/git", body)
	s.gitRelay(w, st, data, err)
}

// POST /api/git/session-dir {project, session_id} — resolve the absolute working
// directory a session acts in for a project (its isolated worktree when a
// session_id is given, else the project checkout). Used by the editor to open the
// same tree the session's agent edits.
func (s *server) handleGitSessionDir(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Project   string `json:"project"`
		SessionID string `json:"session_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Project == "" {
		writeJSONError(w, http.StatusBadRequest, "project required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	body, _ := json.Marshal(map[string]any{"project": req.Project, "session_id": req.SessionID})
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/workspace/session-dir", body)
	s.gitRelay(w, st, data, err)
}

// GET /api/git/org-repos?host=&owner= — list the repos under an owner/org on a git
// host (provider-agnostic), so the wizard can pick a workspace to bulk-clone.
func (s *server) handleGitOrgRepos(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	host := strings.TrimSpace(r.URL.Query().Get("host"))
	owner := strings.TrimSpace(r.URL.Query().Get("owner"))
	if host == "" || owner == "" {
		writeJSONError(w, http.StatusBadRequest, "host and owner required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	path := "/v1/workspace/org-repos?host=" + url.QueryEscape(host) + "&owner=" + url.QueryEscape(owner)
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodGet, path, nil)
	s.gitReposRelay(w, st, data, err)
}

// POST /api/git/clone-org {host, owner, repos:[{name, clone_url}]} — bulk-clone a
// selection of a workspace's repos. Cloning many repos can take a while, so it
// gets a longer deadline than a normal socket call.
func (s *server) handleGitCloneOrg(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Host  string `json:"host"`
		Owner string `json:"owner"`
		Repos []struct {
			Name     string `json:"name"`
			CloneURL string `json:"clone_url"`
		} `json:"repos"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Host == "" || req.Owner == "" || len(req.Repos) == 0 {
		writeJSONError(w, http.StatusBadRequest, "host, owner and repos[] required")
		return
	}
	if len(req.Repos) > 100 {
		writeJSONError(w, http.StatusBadRequest, "too many repos (max 100 per request)")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), cloneOrgTimeout)
	defer cancel()
	body, _ := json.Marshal(req)
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/workspace/clone-org", body)
	s.gitCloneOrgRelay(w, st, data, err)
}
