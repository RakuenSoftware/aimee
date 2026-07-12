package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"
)

type workflowSessionInfo struct {
	ID            int    `json:"id"`
	Template      string `json:"template"`
	Channel       string `json:"channel"`
	Status        string `json:"status"`
	CurrentPhase  int    `json:"current_phase"`
	CurrentTurn   int    `json:"current_turn"`
	PhaseCount    int    `json:"phase_count"`
	TurnsInPhase  int    `json:"turns_in_phase"`
	PhaseName     string `json:"phase_name"`
	ExpectedAgent string `json:"expected_agent"`
	ExpectedRole  string `json:"expected_role"`
	PausedReason  string `json:"paused_reason"`
	CreatedAt     string `json:"created_at"`
	UpdatedAt     string `json:"updated_at"`
	NextPrompt    string `json:"next_prompt,omitempty"`
	RecentContext string `json:"recent_context,omitempty"`
}

func (s *server) mcpStructured(r *http.Request, tool string, arguments map[string]any) (json.RawMessage, error) {
	if arguments == nil {
		arguments = map[string]any{}
	}
	resp, err := s.socketCallForRequest(r, map[string]any{
		"method":    "mcp.call",
		"tool":      tool,
		"arguments": arguments,
	})
	if err != nil {
		return nil, err
	}
	raw, ok := resp["structuredContent"]
	if !ok {
		return nil, fmt.Errorf("server response missing structuredContent")
	}
	return raw, nil
}

func writeJSONError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func (s *server) socketCallForRequest(r *http.Request, req map[string]any) (map[string]json.RawMessage, error) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	return s.rpcV1Call(ctx, req)
}

// methodRoute is the first-class /v1 route backing a dispatch method.
type methodRoute struct {
	verb string
	path string
}

// methodRoutes maps each dispatch method webchat calls to its first-class /v1
// route. The server retired the generic POST /v1/rpc bridge once every method
// gained a real route (src/server/server_http_routes.inc); these mirror that
// table. GET routes take no body; POST routes carry their args in the body. The
// server's rh_dispatch_op returns the dispatch envelope ({status,data,...})
// byte-for-byte, so every caller's response parsing is unchanged.
var methodRoutes = map[string]methodRoute{
	// Dashboard read views (GET, no args).
	"dashboard.all":          {http.MethodGet, "/v1/dashboard/all"},
	"dashboard.audit":        {http.MethodGet, "/v1/dashboard/audit"},
	"dashboard.delegations":  {http.MethodGet, "/v1/dashboard/delegations"},
	"dashboard.metrics":      {http.MethodGet, "/v1/dashboard/metrics"},
	"dashboard.logs":         {http.MethodGet, "/v1/dashboard/logs"},
	"dashboard.plans":        {http.MethodGet, "/v1/dashboard/plans"},
	"dashboard.traces":       {http.MethodGet, "/v1/dashboard/traces"},
	"dashboard.plugins":      {http.MethodGet, "/v1/dashboard/plugins"},
	"dashboard.memory_stats": {http.MethodGet, "/v1/dashboard/memory_stats"},
	"dashboard.onboard":      {http.MethodGet, "/v1/dashboard/onboard"},
	"plugin.list":            {http.MethodGet, "/v1/plugins"},
	"plugin.enable":          {http.MethodPost, "/v1/plugins/enable"},
	"plugin.disable":         {http.MethodPost, "/v1/plugins/disable"},
	"agent.list":             {http.MethodGet, "/v1/agent/list"},
	"agent.stats":            {http.MethodGet, "/v1/agent/stats"},
	"agent.add":              {http.MethodPost, "/v1/agent/add"},
	"agent.remove":           {http.MethodPost, "/v1/agent/remove"},
	"agent.enable":           {http.MethodPost, "/v1/agent/enable"},
	"agent.disable":          {http.MethodPost, "/v1/agent/disable"},
	"agent.probe":            {http.MethodPost, "/v1/agent/probe"},
	"agent.set":              {http.MethodPost, "/v1/agent/set"},
	"hosts.list":             {http.MethodGet, "/v1/hosts"},
	// Subscription-OAuth setup (Claude / Codex "sign in with your plan").
	"agent.cli_oauth_start":    {http.MethodPost, "/v1/agent/cli_oauth_start"},
	"agent.cli_oauth_code":     {http.MethodPost, "/v1/agent/cli_oauth_code"},
	"agent.cli_oauth_poll":     {http.MethodPost, "/v1/agent/cli_oauth_poll"},
	"collab_rules.list":        {http.MethodGet, "/v1/collab_rules"},
	"collab_rules.list_active": {http.MethodGet, "/v1/collab_rules/active"},
	// Mutations + arg-bearing calls (POST, body carries the args; the server
	// overrides the body's method from the matched route).
	"collab_rules.approve":    {http.MethodPost, "/v1/collab_rules/approve"},
	"collab_rules.reject":     {http.MethodPost, "/v1/collab_rules/reject"},
	"collab_rules.retire":     {http.MethodPost, "/v1/collab_rules/retire"},
	"lsp.diagnostics_summary": {http.MethodPost, "/v1/lsp/diagnostics_summary"},
	"index.list":              {http.MethodPost, "/v1/index/list"},
	"mcp.call":                {http.MethodPost, "/v1/mcp/call"},
}

// rpcV1Call dispatches a unary RPC over aimee-server's first-class /v1 HTTP
// routes (resolved from the request's method via methodRoutes) over the UDS.
// rh_dispatch_op returns the dispatch envelope byte-for-byte, so the decoded
// result is identical to the legacy NDJSON socket; the UDS is filesystem-trusted,
// so no bearer is needed. An unmapped method or a genuine transport error falls
// back to the NDJSON socket, so webchat still works against an older socket-only
// server.
func (s *server) rpcV1Call(ctx context.Context, req map[string]any) (map[string]json.RawMessage, error) {
	method, _ := req["method"].(string)
	route, ok := methodRoutes[method]
	if !ok {
		return socketCallContext(ctx, s.cfg.socketPath, req)
	}

	var body []byte
	if route.verb != http.MethodGet {
		var err error
		if body, err = json.Marshal(req); err != nil {
			return nil, err
		}
	}

	st, data, err := s.v1Request(ctx, route.verb, route.path, body)
	if err != nil {
		return socketCallContext(ctx, s.cfg.socketPath, req)
	}
	var msg map[string]json.RawMessage
	if len(data) > 0 {
		if uerr := json.Unmarshal(data, &msg); uerr != nil {
			return nil, fmt.Errorf("decode %s response: %w", route.path, uerr)
		}
	}
	if st != http.StatusOK {
		if rerr := rpcError(msg); rerr != nil {
			return nil, rerr
		}
		return nil, fmt.Errorf("%s status %d", route.path, st)
	}
	if rerr := rpcError(msg); rerr != nil {
		return nil, rerr
	}
	return msg, nil
}

func (s *server) handleAgents(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "agent.list"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		fmt.Fprintf(w, `{"agents":[],"count":0}`)
		return
	}
	agents, ok := resp["agents"]
	if !ok {
		fmt.Fprintf(w, `{"agents":[],"count":0}`)
		return
	}
	var arr []json.RawMessage
	json.Unmarshal(agents, &arr)
	count := len(arr)
	json.NewEncoder(w).Encode(map[string]any{"agents": arr, "count": count})
}

// handleAgentStats proxies GET /api/agents/stats -> RPC agent.stats, returning
// the per-delegate run stats array ({stats:[{name,total_calls,successful_calls,
// failed_calls,success_rate,avg_latency_ms,tokens,cost}]}). Never errors hard:
// an empty roster or a stats-less server yields {"stats":[]} so the page renders.
func (s *server) handleAgentStats(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "agent.stats"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprint(w, `{"stats":[]}`)
		return
	}
	if stats, ok := resp["stats"]; ok {
		// Re-encode through the JSON writer (not string concatenation) so the
		// envelope is always well-formed for the page's getJSON parser.
		_ = json.NewEncoder(w).Encode(map[string]json.RawMessage{"stats": stats})
		return
	}
	fmt.Fprint(w, `{"stats":[]}`)
}

// agentOpHandler proxies a roster mutation (add/remove/enable/disable/probe) to
// its RPC method. The browser POSTs a CLI-style {"args":[...]} body mirroring the
// `aimee agent <op>` argv (e.g. add -> [name,endpoint,model,"--provider","openai",
// "--roles","summarize,format"]; remove/enable/disable/probe -> [name]). The
// server's dispatch envelope is returned verbatim so the UI sees the same
// {status|error,...} shape the CLI would.
func (s *server) agentOpHandler(method string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Args []string `json:"args"`
		}
		if r.Body != nil {
			_ = json.NewDecoder(r.Body).Decode(&body)
		}
		req := map[string]any{"method": method}
		if body.Args != nil {
			req["args"] = body.Args
		}
		resp, err := s.socketCallForRequest(r, req)
		w.Header().Set("Content-Type", "application/json")
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, err.Error())
			return
		}
		// resp is map[string]json.RawMessage; the values marshal back verbatim.
		_ = json.NewEncoder(w).Encode(resp)
	}
}

// cliOauthHandler proxies the browser's subscription-OAuth setup to the
// agent.cli_oauth_{start,code,poll} RPC ops — the Claude / Codex "sign in with
// your subscription" flow that installs + logs the vendor CLI in server-side and
// registers it as a primary-capable agent. The browser POSTs {vendor, session?,
// code?}; only those fields cross. The access token is minted, vaulted, and
// refreshed entirely server-side and never returns to the browser — the response
// carries only the verification URL, an optional device code, the opaque session
// handle, and the login state. `start`'s first call may install the vendor CLI
// (a cold `npm i -g` runs well past the default 10s socket timeout), so each op
// gets a timeout sized to its work.
func (s *server) cliOauthHandler(method string, timeout time.Duration) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Vendor  string `json:"vendor"`
			Session string `json:"session"`
			Code    string `json:"code"`
		}
		if r.Body != nil {
			_ = json.NewDecoder(r.Body).Decode(&body)
		}
		w.Header().Set("Content-Type", "application/json")
		if body.Vendor == "" {
			writeJSONError(w, http.StatusBadRequest, "vendor required")
			return
		}
		req := map[string]any{"method": method, "vendor": body.Vendor}
		if body.Session != "" {
			req["session"] = body.Session
		}
		if body.Code != "" {
			req["code"] = body.Code
		}
		ctx, cancel := context.WithTimeout(r.Context(), timeout)
		defer cancel()
		resp, err := s.rpcV1Call(ctx, req)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, err.Error())
			return
		}
		_ = json.NewEncoder(w).Encode(resp)
	}
}

// argsHaveLiteralKey reports whether a CLI-style `agent add` argv carries a
// LITERAL --key secret (as opposed to a "$ENV" reference, which is not a secret).
// Only a literal key must be sealed into the vault, so only then is the attested
// forwarding path required. Handles both "--key VAL" and "--key=VAL".
func argsHaveLiteralKey(args []string) bool {
	for i, a := range args {
		var val string
		switch {
		case a == "--key":
			if i+1 < len(args) {
				val = args[i+1]
			}
		case strings.HasPrefix(a, "--key="):
			val = strings.TrimPrefix(a, "--key=")
		default:
			continue
		}
		if v := strings.TrimSpace(val); v != "" && !strings.HasPrefix(v, "$") {
			return true
		}
	}
	return false
}

// handleAgentAdd proxies POST /api/agents/add. It mirrors agentOpHandler's
// {"args":[...]} body, but when the argv carries a LITERAL --key the secret must
// be sealed server-side. A plain UDS hop authenticates only as the (root) webchat
// process — an empty, un-writable vault principal — so the seal is refused. That
// one case is forwarded over the attested webuser channel (v1RequestWebuser:
// server.token + X-Aimee-Webuser) so aimee-server classifies the caller as
// webuser:<user>; when that principal holds the vault:write:server capability the
// key lands in the SHARED server vault. The /v1 bearer never enters webchat. A
// keyless add / $VAR reference needs no attested identity and takes the plain path.
func (s *server) handleAgentAdd(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Args []string `json:"args"`
	}
	if r.Body != nil {
		_ = json.NewDecoder(r.Body).Decode(&body)
	}
	w.Header().Set("Content-Type", "application/json")

	req := map[string]any{"method": "agent.add"}
	if body.Args != nil {
		req["args"] = body.Args
	}

	if !argsHaveLiteralKey(body.Args) {
		resp, err := s.socketCallForRequest(r, req)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, err.Error())
			return
		}
		_ = json.NewEncoder(w).Encode(resp)
		return
	}

	payload, err := json.Marshal(req)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "encode request")
		return
	}
	st, data, err := s.v1RequestWebuser(r.Context(), currentUser(r), http.MethodPost, "/v1/agent/add", payload)
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, err.Error())
		return
	}
	var msg map[string]json.RawMessage
	if len(data) > 0 {
		if uerr := json.Unmarshal(data, &msg); uerr != nil {
			writeJSONError(w, http.StatusBadGateway, "decode agent.add response")
			return
		}
	}
	// Surface the server's dispatch error (e.g. the vault refusal) to the UI as
	// {"error": ...}; the Agents page reads res.error.
	if rerr := rpcError(msg); rerr != nil {
		writeJSONError(w, http.StatusBadGateway, rerr.Error())
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, http.StatusBadGateway, fmt.Sprintf("agent add: status %d", st))
		return
	}
	_ = json.NewEncoder(w).Encode(msg)
}

// handleAudit proxies GET /api/audit -> the PAGINATED dashboard.audit route,
// forwarding sanitized limit/offset query params and passing the server's
// {status,data,total,offset,limit} envelope straight through so the Logs page can
// page an arbitrarily large ledger (the response never overflows the server's
// per-request buffer).
func (s *server) handleAudit(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	path := "/v1/dashboard/audit"
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))
	if limit > 0 || offset > 0 {
		path = fmt.Sprintf("%s?limit=%d&offset=%d", path, limit, offset)
	}
	st, data, err := s.v1Request(ctx, http.MethodGet, path, nil)
	w.Header().Set("Content-Type", "application/json")
	if err != nil || st != http.StatusOK || len(data) == 0 {
		fmt.Fprint(w, `{"data":[],"total":0,"offset":0,"limit":0}`)
		return
	}
	w.Write(data)
}

func (s *server) handleDelegations(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "dashboard.delegations"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, "[]")
		return
	}
	if data, ok := resp["data"]; ok {
		w.Write(data)
	} else {
		fmt.Fprintf(w, "[]")
	}
}

func (s *server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "dashboard.metrics"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, "[]")
		return
	}
	if data, ok := resp["data"]; ok {
		w.Write(data)
	} else {
		fmt.Fprintf(w, "[]")
	}
}

func (s *server) handleCollabRulesList(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "collab_rules.list"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, "[]")
		return
	}
	if rules, ok := resp["rules"]; ok {
		w.Write(rules)
	} else {
		fmt.Fprintf(w, "[]")
	}
}

func (s *server) handleCollabRulesActive(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "collab_rules.list_active"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, "{}")
		return
	}
	if active, ok := resp["active"]; ok {
		w.Write(active)
	} else {
		fmt.Fprintf(w, "{}")
	}
}

func (s *server) handleCollabRuleAction(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	idStr := r.PathValue("id")
	action := r.PathValue("action")
	id, err := strconv.Atoi(idStr)
	if err != nil || id <= 0 {
		http.Error(w, `{"error":"invalid id"}`, http.StatusBadRequest)
		return
	}
	switch action {
	case "approve", "reject", "retire":
	default:
		http.Error(w, `{"error":"unknown action"}`, http.StatusBadRequest)
		return
	}
	_, serr := s.socketCallForRequest(r, map[string]any{
		"method": "collab_rules." + action,
		"id":     id,
	})
	w.Header().Set("Content-Type", "application/json")
	if serr != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprintf(w, `{"error":%q}`, serr.Error())
		return
	}
	fmt.Fprintf(w, `{"status":"ok"}`)
}

func (s *server) dataArrayHandler(method string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		resp, err := s.socketCallForRequest(r, map[string]any{"method": method})
		w.Header().Set("Content-Type", "application/json")
		if err != nil || resp == nil {
			fmt.Fprintf(w, "[]")
			return
		}
		if data, ok := resp["data"]; ok {
			w.Write(data)
		} else {
			fmt.Fprintf(w, "[]")
		}
	}
}

func (s *server) dataObjectHandler(method string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		resp, err := s.socketCallForRequest(r, map[string]any{"method": method})
		w.Header().Set("Content-Type", "application/json")
		if err != nil || resp == nil {
			fmt.Fprintf(w, "{}")
			return
		}
		if data, ok := resp["data"]; ok {
			w.Write(data)
		} else {
			fmt.Fprintf(w, "{}")
		}
	}
}

type pluginInfo struct {
	Name    string `json:"name"`
	Version string `json:"version,omitempty"`
	Kind    string `json:"kind,omitempty"`
	Enabled bool   `json:"enabled"`
	Source  string `json:"source_path,omitempty"`
}

func (s *server) handlePluginsList(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "plugin.list"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, "[]")
		return
	}
	if data, ok := resp["data"]; ok {
		w.Write(data)
	} else {
		fmt.Fprintf(w, "[]")
	}
}

func (s *server) handlePluginToggle(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	name := r.PathValue("name")
	if name == "" {
		writeJSONError(w, http.StatusBadRequest, "plugin name required")
		return
	}

	resp, err := s.socketCallForRequest(r, map[string]any{"method": "plugin.list"})
	if err != nil || resp == nil {
		writeJSONError(w, http.StatusServiceUnavailable, "failed to load plugins")
		return
	}
	var plugins []pluginInfo
	if data, ok := resp["data"]; ok {
		_ = json.Unmarshal(data, &plugins)
	}
	found := false
	enabled := false
	for _, plugin := range plugins {
		if plugin.Name == name {
			found = true
			enabled = plugin.Enabled
			break
		}
	}
	if !found {
		writeJSONError(w, http.StatusNotFound, "plugin not found")
		return
	}

	method := "plugin.enable"
	if enabled {
		method = "plugin.disable"
	}
	resp, err = s.socketCallForRequest(r, map[string]any{"method": method, "name": name})
	if err != nil || resp == nil {
		writeJSONError(w, http.StatusInternalServerError, "failed to toggle plugin")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	if data, ok := resp["data"]; ok {
		w.Write(data)
	} else {
		_ = json.NewEncoder(w).Encode(map[string]any{"name": name, "enabled": !enabled})
	}
}

func (s *server) handleDashboardAll(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "dashboard.all"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, "{}")
		return
	}
	out := map[string]json.RawMessage{}
	for k, v := range resp {
		if k == "status" {
			continue
		}
		out[k] = v
	}
	// Active sessions are webchat-owned (not in the aimee-server dashboard.all),
	// so inject the current user's sessions for the dashboard's Sessions panel.
	if sessions, serr := s.listChatSessions(currentUser(r)); serr == nil {
		if raw, merr := json.Marshal(sessions); merr == nil {
			out["sessions"] = raw
		}
	}
	json.NewEncoder(w).Encode(out)
}

func (s *server) handleWorkflowByChannel(w http.ResponseWriter, r *http.Request) {
	channel := r.PathValue("channel")
	if channel == "" {
		writeJSONError(w, http.StatusBadRequest, "channel required")
		return
	}

	raw, err := s.mcpStructured(r, "session_list", map[string]any{"limit": 100})
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, err.Error())
		return
	}

	var payload struct {
		Sessions []workflowSessionInfo `json:"sessions"`
	}
	if err := json.Unmarshal(raw, &payload); err != nil {
		writeJSONError(w, http.StatusInternalServerError, "invalid workflow response")
		return
	}

	var selected *workflowSessionInfo
	for i := range payload.Sessions {
		session := &payload.Sessions[i]
		if session.Channel != channel {
			continue
		}
		if session.Status != "active" && session.Status != "paused" {
			continue
		}
		if selected == nil ||
			(session.Status == "active" && selected.Status != "active") ||
			(session.Status == selected.Status && session.UpdatedAt > selected.UpdatedAt) {
			selected = session
		}
	}

	if selected == nil {
		writeJSONError(w, http.StatusNotFound, fmt.Sprintf("workflow session for channel %q not found", channel))
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(selected)
}

func (s *server) handleWorkflowPause(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.Atoi(r.PathValue("id"))
	if err != nil || id <= 0 {
		writeJSONError(w, http.StatusBadRequest, "invalid workflow id")
		return
	}
	var body struct {
		Reason string `json:"reason"`
	}
	_ = json.NewDecoder(r.Body).Decode(&body)
	if body.Reason == "" {
		body.Reason = "manual"
	}

	raw, err := s.mcpStructured(r, "session_pause", map[string]any{
		"id":     id,
		"reason": body.Reason,
	})
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, err.Error())
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.Write(raw)
}

func (s *server) handleLSPDiagnosticsSummary(w http.ResponseWriter, r *http.Request) {
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "lsp.diagnostics_summary"})
	w.Header().Set("Content-Type", "application/json")
	if err != nil || resp == nil {
		fmt.Fprintf(w, `{"errors":0,"warnings":0,"active_servers":0}`)
		return
	}
	out := map[string]int{"errors": 0, "warnings": 0, "active_servers": 0}
	for _, k := range []string{"errors", "warnings", "active_servers"} {
		if v, ok := resp[k]; ok {
			var n float64
			if json.Unmarshal(v, &n) == nil {
				out[k] = int(n)
			}
		}
	}
	json.NewEncoder(w).Encode(out)
}
