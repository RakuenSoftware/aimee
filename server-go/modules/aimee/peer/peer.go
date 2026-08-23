// Package peer implements session peer messaging: the verb by which one aimee
// session addresses another as an equal rather than as a delegate or a client.
//
// Every other inter-agent path in aimee is hierarchical and outbound — a
// session spawns a delegate, drives a provider CLI, or has an editor attach to
// it. This package adds the missing direction: inbound peer delivery, where a
// message from session B becomes an addressable item in session A carrying
// provenance A can trust and a route back.
//
// # Ownership
//
// This is a Go-owned family, per docs/dev/GO_REWRITE.md ("C is not an ownership
// boundary in the target architecture"). It solely owns the INBOXES and the
// CROSS-OWNER GRANTS, and nothing else.
//
// It does NOT own the session directory. db1's server_sessions table already
// holds every column a peer directory needs, so a second one would mean two
// tables disagreeing about which sessions exist, and the interesting failure is
// the one row where they disagree. The directory is read through the module's
// DirectorySource hook; the in-memory map below is a test fallback and a
// bring-up path, never a second source of truth.
//
// Resolution has ONE path: binding a DirectorySource binds it into the registry
// too, so a caller arriving over the bus and one arriving over HTTP get the same
// answer. Until it is bound the map answers, and a negative from it is a guess
// rather than a fact -- a known gap, not a design.
//
// Turn arbitration stays C-owned in the presence registry and is READ across
// that boundary through the Live hook, never written.
//
// The split is safe because peer delivery is pull-based: Send appends to an
// inbox and returns, and the receiver drains it at a moment of its own choosing
// via Take. Delivery is never atomic with a turn, so it does not need to live
// where the turn lock lives.
//
// # Ask lives here, not on the bus
//
// Ask below blocks on the caller's OWN inbox, never on the peer's turn, so it
// cannot wedge a peer that is busy serving its own operator. That makes it safe
// in process — but it is deliberately NOT the module's bus surface. The bus
// refuses a seventeenth concurrent invocation outright (moduleMaxInFlight = 16,
// no queue), so a stage that blocked while waiting for another agent would wedge
// the whole module. Over the bus an ask is split: Send with ExpectReply records
// the wait-for edge and returns, and the caller polls its own inbox.
//
// # The authority rule
//
// A PEER IS NOT AN OPERATOR. Nothing here grants a sender any capability over a
// receiver. A message is data: callers rendering one into a model's context
// must keep it in a role distinct from that model's operator, so it can never
// be mistaken for an instruction from the person the session belongs to. This
// package's contribution is making the provenance unforgeable — FromSession,
// FromOwner and FromLabel are read out of the sender's own directory entry
// under the lock, never taken from an argument — so that separation has
// something solid to rest on.
//
// # Durability
//
// Inboxes live in memory and do not survive a restart of this process. A
// message outlives its receiver's detachment (the entry is kept alive by a
// non-empty inbox) but not a process bounce.
//
// Durability arrives through the postgres module's generic storage wire under
// owner "aimee", with peer_inbox and peer_grants numbered 1 and 2 explicitly.
// Nothing in this API changes when it lands.
package peer

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// Bounds. These are the ping-pong and context-bloat defences; they are the
// reason a runaway exchange between two agents cannot consume the appliance.
const (
	// SessionsMax caps the addressable directory. Unbounded, a caller that
	// registers in a loop grows the registry until the process dies, and the
	// bounds below would be defending a room whose walls are missing.
	//
	// Generous rather than tight: this is a ceiling against runaway, not a
	// capacity plan. The C registry this replaced held 64, which was an array
	// size rather than a judgement.
	SessionsMax = 256
	// GrantsMax caps cross-owner peering grants for the same reason. A grant is
	// a capability, so an unbounded grant table is also an unbounded
	// authorization surface.
	GrantsMax = 128
	// InboxMax caps the messages one session may have waiting. Overflow is
	// refused and counted, never silently dropped.
	InboxMax = 32
	// DefaultMaxHops caps how many messages one conversation may carry. A
	// reply at the ceiling is refused, which is what terminates a ping-pong.
	DefaultMaxHops = 16
	// MaxTextBytes caps one message body. Over-long text is an explicit
	// error, never a truncation — a byte prefix must never be mistaken for
	// the message someone meant to send.
	MaxTextBytes = 8192
	// DefaultWaitExpiry bounds an unattended wait-for edge. Long enough that a
	// slow peer is not mistaken for a dead caller, short enough that a caller
	// that died does not poison the path it was asking along.
	DefaultWaitExpiry = 5 * time.Minute
	// PreviewBytes bounds the body excerpt carried on the live notification
	// so an observer sees what arrived without the notification becoming the
	// artifact. The inbox always holds the full text.
	PreviewBytes = 512
)

var (
	ErrNoPeer        = errors.New("peer: no such peer session")
	ErrUnknownSender = errors.New("peer: unknown sender session")
	ErrDenied        = errors.New("peer: cross-owner addressing requires a grant")
	ErrInboxFull     = errors.New("peer: peer inbox is full")
	ErrHopLimit      = errors.New("peer: conversation reached the hop ceiling")
	ErrCycle         = errors.New("peer: ask would close a wait-for cycle")
	ErrTimeout       = errors.New("peer: ask timed out waiting for a reply")
	ErrSelf          = errors.New("peer: a session cannot address itself")
	ErrTooLong       = errors.New("peer: message body exceeds MaxTextBytes")
	ErrLabelTaken    = errors.New("peer: label already held by another session of this owner")
	ErrBadRequest    = errors.New("peer: invalid request")
	// ErrOwnerMismatch is a session id already held by a DIFFERENT principal.
	// Distinct from ErrBadRequest because the repair is different: one means a
	// required value was omitted, this means the caller is not who the session
	// belongs to. Reported as one message, an operator hunts for a malformed
	// argument that is not there.
	ErrOwnerMismatch = errors.New("peer: session belongs to a different owner")
)

// SentinelErrorCount is how many sentinel errors this package declares, across
// this file and channel.go. The wire's mapping test asserts against it, so an
// error added without a status mapping fails rather than falling to a default
// and being reported as something it is not.
const SentinelErrorCount = 21

var (
	ErrShutdown     = errors.New("peer: registry shut down while waiting")
	ErrRegistryFull = errors.New("peer: session registry is full")
	ErrGrantsFull   = errors.New("peer: grant table is full")
	// ErrDirectoryUnavailable is the directory failing to ANSWER, which is not
	// the same as answering that a session is gone. A departed session is a
	// fact to act on; an unreachable directory is a reason to try again. Told
	// apart because collapsing them turns a transient fault into a terminal
	// refusal, and would let a sweep destroy mail on a momentary outage.
	ErrDirectoryUnavailable = errors.New("peer: directory did not answer")
)

// Message is one peer message and its envelope. Everything before Text is
// stamped by the registry; a sender supplies only the body.
type Message struct {
	ID             string `json:"message_id"`
	CorrelationID  string `json:"correlation_id,omitempty"`
	ConversationID string `json:"conversation_id"`
	FromSession    string `json:"from_session"`
	FromOwner      string `json:"from_owner"`
	FromLabel      string `json:"from_label,omitempty"`
	// OriginSession is the session that opened the conversation, and so the
	// one whose budget the whole exchange is charged to: a runaway ping-pong
	// exhausts an allowance someone is watching rather than the appliance's.
	// Send stamps the sender; Reply propagates whatever it is answering, so
	// the origin survives every hop.
	OriginSession string    `json:"origin_session"`
	Hop           int       `json:"hop"`
	IsReply       bool      `json:"is_reply"`
	SentAt        time.Time `json:"sent_at"`
	Text          string    `json:"text"`
}

// Preview is the bounded excerpt carried on a live notification. It is
// explicitly an excerpt, never the message: Text remains authoritative.
//
// PreviewBytes is a BYTE bound, but the cut is taken at a rune boundary. A
// plain m.Text[:PreviewBytes] splits a multi-byte character whenever one
// straddles the limit, and the resulting invalid UTF-8 travels into a live
// notification and from there into a reader's context. Peer message bodies are
// the most likely thing in this system to contain multi-byte characters, so the
// straddle is the common case rather than the exotic one.
//
// Backing off to the boundary means a preview can be up to three bytes shorter
// than the bound. That is the correct trade: the bound exists to stop a
// notification growing without limit, not to be met exactly.
func (m Message) Preview() string {
	if len(m.Text) <= PreviewBytes {
		return m.Text
	}
	cut := PreviewBytes
	for cut > 0 && !utf8.RuneStart(m.Text[cut]) {
		cut--
	}
	return m.Text[:cut]
}

// DirEntry is one row of the peer directory — enough for a model to choose who
// to address without knowing session ids by heart.
type DirEntry struct {
	SessionID string `json:"session_id"`
	Owner     string `json:"owner"`
	Label     string `json:"label,omitempty"`
	Surface   string `json:"surface,omitempty"`
	Inbox     int    `json:"inbox"`
	IdleMS    int64  `json:"idle_ms"`
	// Live state read across the C boundary; zero when no Live hook is set.
	Attachments  int    `json:"attachments"`
	TurnInFlight bool   `json:"turn_in_flight"`
	TurnID       string `json:"turn_id,omitempty"`
}

// LiveState is turn/attachment state owned by the C presence registry. This
// package reads it and never writes it.
type LiveState struct {
	Attachments  int
	TurnInFlight bool
	TurnID       string
}

// Options configure a Registry.
type Options struct {
	// Live reports C-owned turn state for the directory. Optional: when nil
	// the directory simply omits those fields rather than guessing.
	Live func(sessionID string) (LiveState, bool)
	// Notify is called after a message is placed in a receiver's inbox, with
	// the receiver's session id. It is how peer traffic reaches the live
	// presence-event stream so an attached human sees agent-to-agent
	// conversation as it happens rather than only in an audit query.
	// Called WITHOUT the registry lock held.
	Notify func(sessionID string, m Message)
	// SessionOwner reports a session's owner principal, or why it cannot.
	//
	// THREE outcomes, because the directory has three. db1's session lookup
	// answers OK, MISSING or a failure, and the Go caller side preserves the
	// status word rather than collapsing it. An implementation returns
	// ErrNoPeer for a definite absence and any other error for "I could not
	// answer" -- returning ErrNoPeer on a transport failure would report a live
	// session as gone.
	//
	// Set it for the same reason as Resolve, and it is the more dangerous of the
	// two to leave unset. This registry only holds an entry for a session it has
	// SEEN -- one that registered in process, or that someone has messaged. A
	// session db1 knows about but nobody has yet written to has no entry here,
	// and answering "no such session" for it inverts the very distinction the
	// inbox stage exists to make: the truthful answer is "no mail", an empty
	// inbox, not a missing peer. A caller told the latter stops asking.
	SessionOwner func(sessionID string) (string, error)
	// MaxHops overrides DefaultMaxHops when > 0.
	MaxHops int
	// now is injectable for tests.
	now func() time.Time
}

type session struct {
	id       string
	owner    string
	label    string
	surface  string
	lastSeen time.Time
	inbox    []Message
	dropped  uint64
	// waitingOn is the peer this session's in-flight ask is waiting on ("" when
	// not asking). These edges form the wait-for graph.
	waitingOn string
	// waitingUntil expires the edge above. An edge past its expiry is treated
	// as absent by cycle detection: a caller that died must not leave a path
	// permanently un-askable.
	waitingUntil time.Time
	// registered is false once Unregister ran but a non-empty inbox is keeping
	// the entry alive.
	registered bool
}

// live reports whether the entry should still exist. A pending inbox keeps a
// detached session's entry alive: a message addressed to a session that went
// quiet must still be there when it comes back, and dropping the entry would
// discard it.
func (s *session) live() bool { return s.registered || len(s.inbox) > 0 }

// Registry is the broker. Every peer message passes through one of these, which
// is what makes governance and capture a property of the path rather than
// something each call site must remember.
type Registry struct {
	mu       sync.Mutex
	cond     *sync.Cond
	sessions map[string]*session
	grants   map[[2]string]bool
	maxHops  int
	seq      uint64
	opts     Options
	closed   bool
	// channels maps a channel name to its member set. Membership only; a
	// channel owns no history and no transport of its own (see channel.go).
	channels map[string]map[string]bool
}

// Close shuts the registry down, resolving every in-flight Ask with
// ErrShutdown and refusing further delivery.
//
// The wait-for graph is deliberately not durable — it describes calls that will
// never be answered again, so a restart is right to clear it. But clearing the
// edges without resolving the callers waiting on them is precisely the failure
// that presents as a hang: the graph looks healthy and the caller never
// returns. So shutdown wakes the waiters rather than only dropping the state.
func (r *Registry) Close() {
	r.mu.Lock()
	r.closed = true
	for _, s := range r.sessions {
		clearWaiting(s)
	}
	r.cond.Broadcast()
	r.mu.Unlock()
}

// New builds a Registry.
func New(opts Options) *Registry {
	if opts.MaxHops <= 0 {
		opts.MaxHops = DefaultMaxHops
	}
	if opts.now == nil {
		opts.now = time.Now
	}
	r := &Registry{
		sessions: make(map[string]*session),
		grants:   make(map[[2]string]bool),
		channels: make(map[string]map[string]bool),
		maxHops:  opts.MaxHops,
		opts:     opts,
	}
	r.cond = sync.NewCond(&r.mu)
	return r
}

func (r *Registry) nextID(prefix string) string {
	r.seq++
	return fmt.Sprintf("%s-%d", prefix, r.seq)
}

// ---- directory -----------------------------------------------------------

// Register makes a session addressable. Calling it again for the same id
// refreshes the surface and liveness; the owner is fixed by the first call and
// a conflicting one is refused.
func (r *Registry) Register(sessionID, owner, surface string) error {
	if sessionID == "" {
		return ErrBadRequest
	}
	r.mu.Lock()
	defer r.mu.Unlock()

	s, ok := r.sessions[sessionID]
	if !ok {
		// Refused, not evicted. Evicting to make room would discard an inbox
		// somebody is waiting on, and pick the victim by map order.
		if len(r.sessions) >= SessionsMax {
			return ErrRegistryFull
		}
		r.sessions[sessionID] = &session{
			id: sessionID, owner: owner, surface: surface,
			lastSeen: r.opts.now(), registered: true,
		}
		return nil
	}
	if owner != "" && s.owner != "" && s.owner != owner {
		return ErrOwnerMismatch
	}
	if s.owner == "" {
		s.owner = owner
	}
	if surface != "" {
		s.surface = surface
	}
	s.registered = true
	s.lastSeen = r.opts.now()
	return nil
}

// Unregister drops a session from the directory. Its entry survives while its
// inbox is non-empty so nothing addressed to it is lost.
func (r *Registry) Unregister(sessionID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	s, ok := r.sessions[sessionID]
	if !ok {
		return
	}
	s.registered = false
	clearWaiting(s)
	if !s.live() {
		r.dropFromChannelsLocked(sessionID)
		delete(r.sessions, sessionID)
	}
	r.cond.Broadcast()
}

// SetLabel sets a session's addressable handle. Labels are unique per owner: a
// label already held by a different session of the same owner is refused rather
// than silently reassigned, because a directory that lies about which session is
// "reviewer" is worse than one that refuses the collision.
func (r *Registry) SetLabel(sessionID, label string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	s, ok := r.sessions[sessionID]
	if !ok {
		return ErrNoPeer
	}
	if label != "" {
		for id, other := range r.sessions {
			if id != sessionID && other.owner == s.owner && other.label == label {
				return ErrLabelTaken
			}
		}
	}
	s.label = label
	return nil
}

// SetSessionOwner binds the authoritative owner lookup, for the same reason and
// at the same moment as SetResolver. Passing nil restores the in-memory
// fallback.
func (r *Registry) SetSessionOwner(owner func(sessionID string) (string, error)) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.opts.SessionOwner = owner
}

// Owner reports a session's owner principal, preferring the directory that owns
// it. A caller needs this to scope a label lookup to the right principal, and
// reading it from the local map would scope against a copy.
func (r *Registry) Owner(sessionID string) (string, error) {
	r.mu.Lock()
	lookup := r.opts.SessionOwner
	s, local := r.sessions[sessionID]
	var localOwner string
	if local {
		localOwner = s.owner
	}
	r.mu.Unlock()

	if lookup != nil {
		owner, err := lookup(sessionID)
		if err == nil {
			return owner, nil
		}
		if errors.Is(err, ErrNoPeer) {
			// A definite absence. A local entry means we hold mail for a session
			// that no longer exists, which is undeliverable rather than
			// addressable.
			return "", ErrNoPeer
		}
		// The directory could not answer. Saying "gone" here would be a guess
		// with the shape of a fact, and the caller's correct response to "gone"
		// is to stop.
		return "", fmt.Errorf("%w: %v", ErrDirectoryUnavailable, err)
	}
	if local {
		return localOwner, nil
	}
	return "", ErrNoPeer
}

// Lookup resolves (owner, label) to a session id. An empty owner searches every
// entry.
func (r *Registry) Lookup(owner, label string) (string, bool) {
	if label == "" {
		return "", false
	}
	// Labels are THIS module's state, so the local map is authoritative for them
	// and there is no second answer to reconcile. What is not local is whether the
	// session behind a label still exists, so a match is confirmed against the
	// directory before it is handed out: a label pointing at a departed session
	// would otherwise send a caller somewhere unreachable.
	r.mu.Lock()
	var candidate string
	for id, sess := range r.sessions {
		if sess.label != label {
			continue
		}
		if owner != "" && sess.owner != owner {
			continue
		}
		candidate = id
		break
	}
	r.mu.Unlock()

	if candidate == "" {
		return "", false
	}
	// Confirm the session behind the label still exists. An unanswerable
	// directory is deliberately treated as unresolved rather than absent: the
	// caller retries instead of concluding the peer is gone.
	if _, err := r.Owner(candidate); err != nil {
		return "", false
	}
	return candidate, true
}

// Directory lists addressable peers, newest activity first. An empty owner
// lists every entry; callers enforcing same-owner visibility pass the caller's
// principal.
func (r *Registry) Directory(owner string) []DirEntry {
	r.mu.Lock()
	entries := make([]DirEntry, 0, len(r.sessions))
	now := r.opts.now()
	for _, s := range r.sessions {
		if owner != "" && s.owner != owner {
			continue
		}
		e := DirEntry{
			SessionID: s.id, Owner: s.owner, Label: s.label, Surface: s.surface,
			Inbox: len(s.inbox), IdleMS: now.Sub(s.lastSeen).Milliseconds(),
		}
		if e.IdleMS < 0 {
			e.IdleMS = 0
		}
		entries = append(entries, e)
	}
	live := r.opts.Live
	r.mu.Unlock()

	// Enrich outside the lock: Live crosses a process boundary and must never
	// be called while holding the registry mutex.
	if live != nil {
		for i := range entries {
			if st, ok := live(entries[i].SessionID); ok {
				entries[i].Attachments = st.Attachments
				entries[i].TurnInFlight = st.TurnInFlight
				entries[i].TurnID = st.TurnID
			}
		}
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].IdleMS != entries[j].IdleMS {
			return entries[i].IdleMS < entries[j].IdleMS
		}
		return entries[i].SessionID < entries[j].SessionID
	})
	return entries
}

// ---- cross-owner grants --------------------------------------------------

// Grant lets fromOwner address toOwner's sessions. Grants are DIRECTED: this
// says nothing about the reverse, which needs its own grant.
func (r *Registry) Grant(fromOwner, toOwner string) error {
	if fromOwner == "" || toOwner == "" {
		return ErrBadRequest
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	key := [2]string{fromOwner, toOwner}
	if !r.grants[key] && len(r.grants) >= GrantsMax {
		return ErrGrantsFull
	}
	r.grants[key] = true
	return nil
}

// Revoke withdraws a grant. Reports whether one was there to withdraw.
func (r *Registry) Revoke(fromOwner, toOwner string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	key := [2]string{fromOwner, toOwner}
	had := r.grants[key]
	delete(r.grants, key)
	return had
}

// GrantExists reports whether fromOwner may address toOwner.
func (r *Registry) GrantExists(fromOwner, toOwner string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.grants[[2]string{fromOwner, toOwner}]
}

// MaxHops reports the conversation hop ceiling.
func (r *Registry) MaxHops() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.maxHops
}

// SetMaxHops sets the hop ceiling; <= 0 restores the default.
func (r *Registry) SetMaxHops(n int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if n <= 0 {
		n = DefaultMaxHops
	}
	r.maxHops = n
}

// ---- delivery ------------------------------------------------------------

// authorized reports whether from may address to. Same-owner peering is
// implicit; crossing owners needs a directed grant. Caller holds the lock.
func (r *Registry) authorized(from, to *session) bool {
	if from.owner == to.owner {
		return true
	}
	return r.grants[[2]string{from.owner, to.owner}]
}

// dropFromChannelsLocked removes a session from every channel it belongs to,
// deleting a channel that empties. Called wherever a session's ENTRY is
// removed: membership pointing at a session that no longer exists would make
// every later ChannelSend report a recipient that can never receive, which is
// the in-memory form of the orphan-row problem the durable inbox has.
// Caller holds the lock.
func (r *Registry) dropFromChannelsLocked(sessionID string) {
	for name, members := range r.channels {
		if !members[sessionID] {
			continue
		}
		delete(members, sessionID)
		if len(members) == 0 {
			delete(r.channels, name)
		}
	}
}

// sortStrings orders a small slice in place. Channel listings are sorted so a
// caller sees a stable answer rather than Go's randomized map order.
func sortStrings(v []string) { sort.Strings(v) }

// waitingLive reports whether a session holds an unexpired wait-for edge.
// Caller holds the lock.
func (r *Registry) waitingLive(s *session) bool {
	if s.waitingOn == "" {
		return false
	}
	if !s.waitingUntil.IsZero() && r.opts.now().After(s.waitingUntil) {
		return false
	}
	return true
}

// setWaiting records a wait-for edge with an expiry. Caller holds the lock.
func (r *Registry) setWaiting(s *session, on string, expiry time.Duration) {
	if expiry <= 0 {
		expiry = DefaultWaitExpiry
	}
	s.waitingOn = on
	s.waitingUntil = r.opts.now().Add(expiry)
}

// clearWaiting drops a session's wait-for edge. Caller holds the lock.
func clearWaiting(s *session) {
	s.waitingOn = ""
	s.waitingUntil = time.Time{}
}

// CancelWait drops a session's wait-for edge explicitly. A caller that gives up
// on an ask should call this rather than let the edge expire, so the path it was
// asking along is immediately askable again.
func (r *Registry) CancelWait(sessionID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if s, ok := r.sessions[sessionID]; ok {
		clearWaiting(s)
		r.cond.Broadcast()
	}
}

// closesCycle reports whether adding the wait-for edge from→to would close a
// cycle, by walking the existing edges forward from `to`. Caller holds the lock.
func (r *Registry) closesCycle(from, to string) bool {
	cur := to
	for steps := 0; steps <= len(r.sessions); steps++ {
		if cur == from {
			return true
		}
		s, ok := r.sessions[cur]
		if !ok || !r.waitingLive(s) {
			return false // the chain ends in someone who is not waiting
		}
		cur = s.waitingOn
	}
	// More steps than there are sessions means the walk is going in circles.
	// Refusing is the safe answer: a false positive costs a fallback to Send,
	// a missed one costs two wedged sessions.
	return true
}

type deliverSpec struct {
	text           string
	conversationID string
	correlationID  string
	originSession  string
	hop            int
	isReply        bool
	correlateSelf  bool
}

// deliver stamps an envelope onto to's inbox. Caller holds the lock. The
// returned Message must be handed to notify() after the lock is released.
func (r *Registry) deliver(from, to *session, spec deliverSpec) (Message, error) {
	if !r.authorized(from, to) {
		return Message{}, ErrDenied
	}
	if spec.hop < 0 {
		spec.hop = 0
	}
	if spec.hop >= r.maxHops {
		return Message{}, ErrHopLimit
	}
	if len(to.inbox) >= InboxMax {
		to.dropped++
		return Message{}, ErrInboxFull
	}

	m := Message{
		ID:             r.nextID("pmsg"),
		ConversationID: spec.conversationID,
		// Provenance is read from the sender's own entry, never from an
		// argument — this is what makes the envelope unforgeable.
		FromSession:   from.id,
		FromOwner:     from.owner,
		FromLabel:     from.label,
		OriginSession: spec.originSession,
		Hop:           spec.hop,
		IsReply:       spec.isReply,
		SentAt:        r.opts.now(),
		Text:          spec.text,
	}
	if spec.correlateSelf {
		m.CorrelationID = m.ID
	} else {
		m.CorrelationID = spec.correlationID
	}
	if m.ConversationID == "" {
		m.ConversationID = r.nextID("conv")
	}
	if m.OriginSession == "" {
		m.OriginSession = from.id
	}

	to.inbox = append(to.inbox, m)
	to.lastSeen = r.opts.now()
	r.cond.Broadcast()
	return m, nil
}

// notify runs the live-notification hook outside the lock.
func (r *Registry) notify(sessionID string, m Message) {
	if r.opts.Notify != nil {
		r.opts.Notify(sessionID, m)
	}
}

// SendOptions carry the conversation context of a Send.
type SendOptions struct {
	// ConversationID continues an existing conversation; empty opens a new one.
	ConversationID string
	// Hop is the sender's current hop — 0 for an opener. A session relaying a
	// message it received at hop N passes N+1.
	Hop int
	// ExpectReply makes this send the opening half of an ask: the message
	// becomes its own correlation id and a wait-for edge is recorded, so a peer
	// asking back closes a cycle the registry can see and refuse.
	//
	// The edge is recorded HERE, at send time, rather than by a blocking call,
	// because the bus refuses a 17th concurrent invocation outright
	// (moduleMaxInFlight = 16, no queue). A stage that blocked while waiting for
	// a peer would hold one of those sixteen slots for its whole deadline, and a
	// feature whose normal use is "wait for another agent" would wedge the
	// module — with the 17th caller getting a generic Internal status that reads
	// as a crash rather than as backpressure. So the registry owns the cycle
	// check and the caller owns the waiting.
	ExpectReply bool
	// WaitExpiry bounds how long the wait-for edge survives; zero uses
	// DefaultWaitExpiry. A caller that dies mid-ask must not leave an edge
	// behind forever, or every later ask along that path is refused as a cycle
	// that no longer exists.
	WaitExpiry time.Duration
}

func validateText(text string) error {
	if strings.TrimSpace(text) == "" {
		return ErrBadRequest
	}
	if len(text) > MaxTextBytes {
		return ErrTooLong
	}
	return nil
}

// Send delivers a message and returns immediately. It never takes the
// receiver's turn: the message waits in the inbox until the receiver drains it,
// so a turn in flight is neither interrupted nor raced.
func (r *Registry) Send(from, to, text string, opts SendOptions) (Message, error) {
	if from == "" || to == "" {
		return Message{}, ErrBadRequest
	}
	if from == to {
		// A trivial self-feeding loop the hop ceiling would only bound after
		// wasting turns. Refuse it outright.
		return Message{}, ErrSelf
	}
	if err := validateText(text); err != nil {
		return Message{}, err
	}

	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return Message{}, ErrShutdown
	}
	fromS, ok := r.sessions[from]
	if !ok {
		r.mu.Unlock()
		return Message{}, ErrUnknownSender
	}
	toS, ok := r.sessions[to]
	if !ok {
		r.mu.Unlock()
		return Message{}, ErrNoPeer
	}
	// An expecting send is the opening half of an ask, so it takes the same
	// cycle check a blocking ask would have taken — the check belongs with the
	// state, not with whoever happens to be waiting.
	if opts.ExpectReply && r.closesCycle(from, to) {
		r.mu.Unlock()
		return Message{}, ErrCycle
	}
	m, err := r.deliver(fromS, toS, deliverSpec{
		text: text, conversationID: opts.ConversationID, hop: opts.Hop,
		correlateSelf: opts.ExpectReply,
	})
	if err == nil && opts.ExpectReply {
		r.setWaiting(fromS, to, opts.WaitExpiry)
	}
	r.mu.Unlock()
	if err != nil {
		return Message{}, err
	}
	r.notify(to, m)
	return m, nil
}

// Reply answers a message taken from an inbox, routing back to its sender with
// its correlation and conversation ids at the next hop — so a waiting Ask wakes
// and a ping-pong still burns the hop budget.
func (r *Registry) Reply(from string, to Message, text string) (Message, error) {
	if from == "" || to.FromSession == "" {
		return Message{}, ErrBadRequest
	}
	if from == to.FromSession {
		return Message{}, ErrSelf
	}
	if err := validateText(text); err != nil {
		return Message{}, err
	}
	corr := to.CorrelationID
	if corr == "" {
		corr = to.ID
	}

	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return Message{}, ErrShutdown
	}
	fromS, ok := r.sessions[from]
	if !ok {
		r.mu.Unlock()
		return Message{}, ErrUnknownSender
	}
	toS, ok := r.sessions[to.FromSession]
	if !ok {
		r.mu.Unlock()
		return Message{}, ErrNoPeer
	}
	m, err := r.deliver(fromS, toS, deliverSpec{
		text:           text,
		conversationID: to.ConversationID,
		correlationID:  corr,
		originSession:  to.OriginSession,
		hop:            to.Hop + 1,
		isReply:        true,
	})
	r.mu.Unlock()
	if err != nil {
		return Message{}, err
	}
	r.notify(to.FromSession, m)
	return m, nil
}

// Ask sends a question and waits for the peer's correlated reply, bounded by
// ctx. It never takes the peer's turn lock — it waits on its OWN inbox — so an
// ask cannot wedge a peer that is busy serving its own operator.
//
// An ask that would close a cycle in the wait-for graph is refused immediately
// with ErrCycle rather than blocking. The caller can fall back to Send, which
// never blocks and so can never cycle.
//
// ON TIMEOUT THE ASK DEGRADES TO A SEND. ErrTimeout comes back with the sent
// question's id, the question stays in the peer's inbox, and a late reply lands
// in this session's inbox still carrying the original correlation id — so the
// answer is recoverable rather than lost.
func (r *Registry) Ask(ctx context.Context, from, to, text string) (Message, string, error) {
	if from == "" || to == "" {
		return Message{}, "", ErrBadRequest
	}
	if from == to {
		return Message{}, "", ErrCycle // waiting on yourself is the smallest cycle
	}
	if err := validateText(text); err != nil {
		return Message{}, "", err
	}

	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return Message{}, "", ErrShutdown
	}
	fromS, ok := r.sessions[from]
	if !ok {
		r.mu.Unlock()
		return Message{}, "", ErrUnknownSender
	}
	toS, ok := r.sessions[to]
	if !ok {
		r.mu.Unlock()
		return Message{}, "", ErrNoPeer
	}
	// Refuse before blocking, never after.
	if r.closesCycle(from, to) {
		r.mu.Unlock()
		return Message{}, "", ErrCycle
	}
	q, err := r.deliver(fromS, toS, deliverSpec{text: text, correlateSelf: true})
	if err != nil {
		r.mu.Unlock()
		return Message{}, "", err
	}
	// Publish the wait-for edge so a peer asking back closes a cycle we can see.
	r.setWaiting(fromS, to, 0)
	r.mu.Unlock()

	r.notify(to, q)

	// sync.Cond has no context-aware Wait; this goroutine turns ctx expiry into
	// a broadcast so the wait loop below re-checks and gives up.
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		select {
		case <-ctx.Done():
			r.mu.Lock()
			r.cond.Broadcast()
			r.mu.Unlock()
		case <-stop:
		}
	}()

	r.mu.Lock()
	defer r.mu.Unlock()
	for {
		// Re-read every pass: the entry can be dropped while this waits.
		s, ok := r.sessions[from]
		if !ok {
			return Message{}, q.ID, ErrUnknownSender
		}
		for i, m := range s.inbox {
			if !m.IsReply || m.CorrelationID != q.ID {
				continue
			}
			// Consume it: the Ask is the delivery, so it must not also be
			// drained later by Take.
			s.inbox = append(s.inbox[:i], s.inbox[i+1:]...)
			clearWaiting(s)
			if !s.live() {
				r.dropFromChannelsLocked(from)
				delete(r.sessions, from)
			}
			r.cond.Broadcast()
			return m, q.ID, nil
		}
		// Shutdown resolves the waiter rather than leaving it on an edge that
		// will never be answered — see Close.
		if r.closed {
			clearWaiting(s)
			r.cond.Broadcast()
			return Message{}, q.ID, ErrShutdown
		}
		if ctx.Err() != nil {
			clearWaiting(s)
			r.cond.Broadcast()
			return Message{}, q.ID, ErrTimeout
		}
		r.cond.Wait()
	}
}

// ---- the inbox -----------------------------------------------------------

// Inbox returns a copy of the messages waiting for a session, without removing
// any.
func (r *Registry) Inbox(sessionID string) []Message {
	r.mu.Lock()
	defer r.mu.Unlock()
	s, ok := r.sessions[sessionID]
	if !ok {
		return nil
	}
	out := make([]Message, len(s.inbox))
	copy(out, s.inbox)
	return out
}

// Len reports how many messages are waiting.
func (r *Registry) Len(sessionID string) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	if s, ok := r.sessions[sessionID]; ok {
		return len(s.inbox)
	}
	return 0
}

// Dropped reports how many messages were refused because the inbox was full.
// An overflow is always counted, so a peer that stopped being heard is
// diagnosable rather than invisible.
func (r *Registry) Dropped(sessionID string) uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	if s, ok := r.sessions[sessionID]; ok {
		return s.dropped
	}
	return 0
}

// Drain removes up to max messages FIFO and reports how many remain.
//
// The remaining count is the point. A reply carrying only rows cannot tell
// "that was all of it" from "that was the first max of more", and a caller that
// drains once and assumes empty is then wrong in a way nothing corrects: it
// stops asking. Complete and capped are two facts, and one length cannot carry
// both. The count is computed under the same lock as the removal, so it cannot
// be raced by a delivery arriving between the two.
//
// In-process callers can follow up with Len; a caller across the bus cannot
// cheaply, which is why the count travels with the rows.
func (r *Registry) Drain(sessionID string, max int) ([]Message, int) {
	if max <= 0 {
		return nil, r.Len(sessionID)
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	s, ok := r.sessions[sessionID]
	if !ok || len(s.inbox) == 0 {
		return nil, 0
	}
	if max > len(s.inbox) {
		max = len(s.inbox)
	}
	out := make([]Message, max)
	copy(out, s.inbox[:max])
	s.inbox = append(s.inbox[:0], s.inbox[max:]...)
	remaining := len(s.inbox)
	s.lastSeen = r.opts.now()
	// An entry kept alive only by a pending inbox is done once it is drained.
	if !s.live() {
		r.dropFromChannelsLocked(sessionID)
		delete(r.sessions, sessionID)
	}
	return out, remaining
}

// Take removes and returns up to max messages, FIFO. This is the receiver's
// point of control: drain at the head of a turn and peer traffic can never
// preempt one already running.
//
// Prefer Drain where the caller needs to know whether anything was left behind.
func (r *Registry) Take(sessionID string, max int) []Message {
	if max <= 0 {
		return nil
	}
	out, _ := r.Drain(sessionID, max)
	return out
}
