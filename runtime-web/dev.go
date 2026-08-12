package main

import (
	"context"
	"io"
	"net/http"
	"time"
)

// POST /api/dev/submit {proposal_md, workflow?, repo?} — autonomous-development
// intake. Proxies to /v1/dev/submit under the authenticated webuser identity so
// the run is created in the caller's scope. Body is passed through verbatim.
func (s *server) handleDevSubmit(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	body, ok := readBoundedBody(w, r, 1<<20)
	if !ok {
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/dev/submit", body)
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "aimee-server unavailable")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(st)
	_, _ = w.Write(data)
}

// draftTimeout bounds the synchronous one-shot LLM draft. It is well above the
// default socketCallTimeout because the request is held open for the model's
// full generation latency (there is no async job to poll).
const draftTimeout = 95 * time.Second

// POST /api/proposal/draft {prompt, model?} — "Draft with a delegate": a single
// tool-free LLM completion (server-side agent.draft) that returns {text, agent}.
// Proxied under the webuser identity (CAP_DELEGATE, like /api/dev/submit) with an
// extended timeout since it holds the request open for the model latency.
func (s *server) handleProposalDraft(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	const maxBody = 1 << 20
	body, _ := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
	if len(body) > maxBody {
		writeJSONError(w, http.StatusRequestEntityTooLarge, "request too large")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), draftTimeout+5*time.Second)
	defer cancel()
	st, data, err := s.v1RequestWebuserT(ctx, currentUser(r), http.MethodPost,
		"/v1/model/draft", body, draftTimeout)
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, "draft service unavailable")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(st)
	_, _ = w.Write(data)
}
