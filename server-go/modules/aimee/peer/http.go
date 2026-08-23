package peer

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"
)

// AskWaitMax bounds how long one request may block waiting for a peer's reply.
// A request that would hold a connection open indefinitely is a denial of
// service against the server, not a patient client.
const AskWaitMax = 120 * time.Second

// HTTPOptions wire the registry into the /v1 surface.
type HTTPOptions struct {
	// Authorize reports whether this request may act as sessionID. There is no
	// default: without it every handler refuses, because a peer surface that
	// authorizes by accident is worse than one that is switched off.
	Authorize func(r *http.Request, sessionID string) bool
	// Principal returns the owner principal for a request, used to scope the
	// directory to same-owner visibility. Returning "" lists nothing.
	Principal func(r *http.Request) string
	// AdminAllowed reports whether a request may manage cross-owner grants.
	// Absent, grant management is refused.
	AdminAllowed func(r *http.Request) bool
}

// Handler returns the /v1 routes for peer messaging:
//
//	GET  /v1/sessions/peers              the directory, scoped to the caller
//	POST /v1/sessions/{id}/peer          send, or ask when wait_ms > 0
//	POST /v1/sessions/{id}/peer/reply    reply to a message taken from the inbox
//	GET  /v1/sessions/{id}/inbox         read pending messages, removing none
//	POST /v1/sessions/{id}/inbox/take    drain, removing what it returns
//	POST /v1/sessions/{id}/label         set this session's addressable handle
//	POST /v1/peers/grants                grant or revoke cross-owner peering
func (r *Registry) Handler(opts HTTPOptions) http.Handler {
	mux := http.NewServeMux()
	h := &httpAPI{reg: r, opts: opts}
	mux.HandleFunc("GET /v1/sessions/peers", h.directory)
	mux.HandleFunc("POST /v1/sessions/{id}/peer", h.send)
	mux.HandleFunc("POST /v1/sessions/{id}/peer/reply", h.reply)
	mux.HandleFunc("GET /v1/sessions/{id}/inbox", h.inbox)
	mux.HandleFunc("POST /v1/sessions/{id}/inbox/take", h.take)
	mux.HandleFunc("POST /v1/sessions/{id}/label", h.label)
	mux.HandleFunc("POST /v1/peers/grants", h.grants)
	return mux
}

type httpAPI struct {
	reg  *Registry
	opts HTTPOptions
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// statusFor maps a registry error to the status a client should act on. The
// distinctions matter: 409 says "restructure the conversation", 429 says "you
// are over a budget", 403 says "you need a grant".
func statusFor(err error) int {
	switch {
	case errors.Is(err, ErrNoPeer), errors.Is(err, ErrUnknownSender):
		return http.StatusNotFound
	case errors.Is(err, ErrDenied):
		return http.StatusForbidden
	case errors.Is(err, ErrInboxFull), errors.Is(err, ErrHopLimit):
		return http.StatusTooManyRequests
	case errors.Is(err, ErrCycle), errors.Is(err, ErrLabelTaken):
		return http.StatusConflict
	case errors.Is(err, ErrTooLong):
		return http.StatusRequestEntityTooLarge
	default:
		return http.StatusBadRequest
	}
}

// authorized gates every session-scoped route. A missing Authorize hook fails
// closed with 503 rather than defaulting to open.
func (h *httpAPI) authorized(w http.ResponseWriter, r *http.Request, sessionID string) bool {
	if h.opts.Authorize == nil {
		writeErr(w, http.StatusServiceUnavailable, "peer messaging is not configured")
		return false
	}
	if sessionID == "" {
		writeErr(w, http.StatusBadRequest, "session id required")
		return false
	}
	if !h.opts.Authorize(r, sessionID) {
		writeErr(w, http.StatusForbidden, "not permitted to act as this session")
		return false
	}
	return true
}

func (h *httpAPI) directory(w http.ResponseWriter, r *http.Request) {
	if h.opts.Principal == nil {
		writeErr(w, http.StatusServiceUnavailable, "peer messaging is not configured")
		return
	}
	// Same-owner visibility: a caller sees the peers of its own principal.
	// Cross-owner reachability is a capability, not a default.
	owner := h.opts.Principal(r)
	if owner == "" {
		writeJSON(w, http.StatusOK, map[string]any{"peers": []DirEntry{}})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"peers": h.reg.Directory(owner)})
}

type sendRequest struct {
	To             string `json:"to"`
	ToLabel        string `json:"to_label"`
	Text           string `json:"text"`
	ConversationID string `json:"conversation_id"`
	Hop            int    `json:"hop"`
	WaitMS         int    `json:"wait_ms"`
}

// resolveTarget accepts either an explicit session id or a label, resolved
// within the sender's own owner so a label can never address across principals
// by accident.
func (h *httpAPI) resolveTarget(from string, req sendRequest) (string, bool) {
	if req.To != "" {
		return req.To, true
	}
	if req.ToLabel == "" {
		return "", false
	}
	// Ask the registry rather than reaching into its map: the owner must come
	// from whoever owns the directory, and scoping a label lookup against a
	// local copy scopes it against the wrong thing.
	owner, err := h.reg.Owner(from)
	if err != nil {
		return "", false
	}
	return h.reg.Lookup(owner, req.ToLabel)
}

func (h *httpAPI) send(w http.ResponseWriter, r *http.Request) {
	from := r.PathValue("id")
	if !h.authorized(w, r, from) {
		return
	}
	var req sendRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, MaxTextBytes*2)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid request body")
		return
	}
	to, ok := h.resolveTarget(from, req)
	if !ok {
		writeErr(w, http.StatusNotFound, "no such peer")
		return
	}

	if req.WaitMS <= 0 {
		m, err := h.reg.Send(from, to, req.Text, SendOptions{
			ConversationID: req.ConversationID, Hop: req.Hop,
		})
		if err != nil {
			writeErr(w, statusFor(err), err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"status": "sent", "message": m})
		return
	}

	wait := time.Duration(req.WaitMS) * time.Millisecond
	if wait > AskWaitMax {
		wait = AskWaitMax
	}
	ctx, cancel := context.WithTimeout(r.Context(), wait)
	defer cancel()

	reply, askID, err := h.reg.Ask(ctx, from, to, req.Text)
	switch {
	case err == nil:
		writeJSON(w, http.StatusOK, map[string]any{
			"status": "answered", "message_id": askID, "reply": reply,
		})
	case errors.Is(err, ErrTimeout):
		// Not a failure: the ask degraded to a send. The question is still in
		// the peer's inbox and a late reply will arrive correlated, so the
		// caller is told what to correlate on rather than given an error.
		writeJSON(w, http.StatusOK, map[string]any{
			"status": "timeout", "message_id": askID,
			"note": "question delivered; a late reply arrives in this session's inbox with this correlation_id",
		})
	default:
		writeErr(w, statusFor(err), err.Error())
	}
}

type replyRequest struct {
	To   Message `json:"to"`
	Text string  `json:"text"`
}

func (h *httpAPI) reply(w http.ResponseWriter, r *http.Request) {
	from := r.PathValue("id")
	if !h.authorized(w, r, from) {
		return
	}
	var req replyRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, MaxTextBytes*4)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid request body")
		return
	}
	m, err := h.reg.Reply(from, req.To, req.Text)
	if err != nil {
		writeErr(w, statusFor(err), err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "sent", "message": m})
}

func (h *httpAPI) inbox(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !h.authorized(w, r, id) {
		return
	}
	msgs := h.reg.Inbox(id)
	if msgs == nil {
		msgs = []Message{}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"messages": msgs, "dropped": h.reg.Dropped(id),
	})
}

func (h *httpAPI) take(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !h.authorized(w, r, id) {
		return
	}
	var req struct {
		Max int `json:"max"`
	}
	// A body is optional here; absent means "drain everything waiting".
	_ = json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&req)
	if req.Max <= 0 {
		req.Max = InboxMax
	}
	msgs := h.reg.Take(id, req.Max)
	if msgs == nil {
		msgs = []Message{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"messages": msgs})
}

// label sets a session's addressable handle.
//
// This route was briefly removed on the reading that a label is db1's
// server_sessions.title, so writing one here would be a second write path to
// another module's state. The reasoning was right and the premise was wrong:
// that column has NO writer. The session insert stores the literal empty string
// and nothing updates it, so deferring to db1 would have left peer addressing
// with nobody able to set a name at all.
//
// A label is not a session title. A title is a display name; a label is the
// handle peer messaging is addressed by, and it is per-session state of exactly
// the kind this module already owns beside inboxes. So it is written here, and
// there is exactly one writer.
func (h *httpAPI) label(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !h.authorized(w, r, id) {
		return
	}
	var req struct {
		Label string `json:"label"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid request body")
		return
	}
	if err := h.reg.SetLabel(id, strings.TrimSpace(req.Label)); err != nil {
		writeErr(w, statusFor(err), err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "label": req.Label})
}

func (h *httpAPI) grants(w http.ResponseWriter, r *http.Request) {
	if h.opts.AdminAllowed == nil || !h.opts.AdminAllowed(r) {
		writeErr(w, http.StatusForbidden, "not permitted to manage peering grants")
		return
	}
	var req struct {
		FromOwner string `json:"from_owner"`
		ToOwner   string `json:"to_owner"`
		Revoke    bool   `json:"revoke"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid request body")
		return
	}
	if req.Revoke {
		writeJSON(w, http.StatusOK, map[string]any{
			"status": "revoked", "existed": h.reg.Revoke(req.FromOwner, req.ToOwner),
		})
		return
	}
	if err := h.reg.Grant(req.FromOwner, req.ToOwner); err != nil {
		writeErr(w, statusFor(err), err.Error())
		return
	}
	// A grant is directed; saying so in the response keeps callers from
	// assuming the reverse now works too.
	writeJSON(w, http.StatusOK, map[string]any{
		"status": "granted", "from_owner": req.FromOwner, "to_owner": req.ToOwner,
		"note": "directed: the reverse direction requires its own grant",
	})
}
