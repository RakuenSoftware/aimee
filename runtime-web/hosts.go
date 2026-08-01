package main

// hosts.go: the page-2 host + GPU picker data source. GET /api/hosts proxies the
// aimee-server `hosts.list` op (GET /v1/hosts), which enumerates the local host and
// every registered network host, each with its LIVE GPU inventory (local via a
// popen probe, remote over ssh). Read-only; goes over the filesystem-trusted UDS
// (no bearer), like the agent-roster proxies.
//
// NOTE: remote hosts are probed sequentially server-side, each bounded by ssh's
// ConnectTimeout. Reachable hosts answer in well under the socket-call timeout;
// several simultaneously-unreachable remote hosts could exceed it. Parallelizing
// the server-side probe (or a longer proxy timeout) is a follow-up if that bites.

import (
	"encoding/json"
	"net/http"
)

// handleHosts proxies GET /api/hosts -> the hosts.list op (GET /v1/hosts). The
// server's {status,hosts:[...]} envelope is returned verbatim.
func (s *server) handleHosts(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	resp, err := s.socketCallForRequest(r, map[string]any{"method": "hosts.list"})
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "hosts: aimee-server unavailable")
		return
	}
	_ = json.NewEncoder(w).Encode(resp)
}
