package aimee

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// SessionDirectory resolves ownership through the aimee session family on the
// authenticated bus. It is used by the live module registry. Missing sessions,
// refused requests and an unavailable store remain distinct outcomes; the
// directory does not open a database or infer ownership from unrelated fields.
type SessionDirectory struct {
	caller   ModuleCaller
	deadline time.Duration
}

// ModuleCaller is the narrow slice of bus.ConcurrentModuleCaller this needs.
//
// The interface keeps status mapping independently testable without opening a
// database or constructing a daemon.
type ModuleCaller interface {
	Call(ctx context.Context, eventKind, stageID uint32, traceID uint64,
		deadline time.Duration, request []byte) ([]byte, error)
}

// Session-family identity. The kind follows from the ref and stage by the bus
// formula, and the catalog tests pin the operation and reply shape.
const (
	// SessionDirectoryPrincipalRef is the aimee domain module's principal.
	SessionDirectoryPrincipalRef uint32 = 30
	// SessionDirectoryStage is the sessions family, id 6 in its catalog.
	SessionDirectoryStage uint32 = 6
	// SessionDirectoryGetOp is AIMEE_DB1_OP_SERVER_SESSION_GET.
	SessionDirectoryGetOp uint32 = 2
	// sessionReplyPrincipal is the index of `principal` in the ten-cell reply:
	// id, client_type, principal, title, created_at, last_activity_at,
	// claude_session_id, outcome, source, chat_key.
	//
	// THE OTHER NINE ARE IGNORED DELIBERATELY, checked rather than assumed. A
	// peer's audit found visibility predicates living in SQL that no caller
	// passed, on the principle that a filter you never named cannot be one you
	// notice dropping -- so the same question was asked here: does addressability
	// depend on anything in this row besides the principal?
	//
	// It does not. The candidate was `outcome`, which looks like a lifecycle
	// state and is not: it holds "success", "partial" or "failure", a judgment
	// about how the work went, written from agent_log entries and only when
	// there are any. Its absence means "no log entries", not "still running",
	// and reading it as liveness would be treating the absence of a row as a
	// value.
	//
	// Liveness is PRESENCE. Closing a session deletes the row
	// (server_session.c calls db1_server_session_delete), after which
	// server_session_get answers Missing and this maps it to ErrNoPeer. So the
	// existence question and the addressability question have the same answer,
	// and there is no second predicate to carry.
	sessionReplyPrincipal = 2
	sessionReplyWidth     = 10
)

// Session-family status words. NOT peerwire.Status: the integers collide and the meanings
// do not. The session family's 1 is MISSING where the peer family's 1 is no_peer, and its 4 is
// FAILED where this module's 4 is hop_limit.
const (
	sessionStatusOK      uint32 = 0
	sessionStatusMissing uint32 = 1
	sessionStatusInvalid uint32 = 2
	sessionStatusTooLong uint32 = 3
	sessionStatusFailed  uint32 = 4
)

// ErrDirectoryRefused is the session family refusing the REQUEST rather than failing: it
// understood, and will not accept what this module sent, which is a bug here
// that no amount of retrying fixes.
//
// An ALIAS of peer.ErrDirectoryRefused, and it lives in that package rather than
// this one because peerwire has to map it and cannot import this package.
// Declared here it was invisible to StatusFor, and worse than unmapped: the
// registry wrapped it as ErrDirectoryUnavailable, so a refusal reached the
// caller as `unavailable` -- the one status meaning retry -- over a request the
// directory will never accept.
var ErrDirectoryRefused = peer.ErrDirectoryRefused

// NewSessionDirectory builds a directory over a bus caller.
func NewSessionDirectory(caller ModuleCaller, deadline time.Duration) (*SessionDirectory, error) {
	if caller == nil {
		return nil, errors.New("aimee: session directory needs a bus caller")
	}
	if deadline <= 0 {
		deadline = 5 * time.Second
	}
	return &SessionDirectory{caller: caller, deadline: deadline}, nil
}

// Owner reports a session's owner principal, or why it cannot.
//
// The four outcomes are kept apart because the caller's correct response to each
// is different: use it, stop, fix the call, retry.
func (d *SessionDirectory) Owner(sessionID string) (string, error) {
	if d == nil || d.caller == nil {
		return "", peer.ErrNoDirectory
	}
	if sessionID == "" {
		// Refused here rather than sent. The session family answers StatusInvalid, so
		// asking would spend a round trip to be told what is already known.
		return "", fmt.Errorf("%w: empty session id", ErrDirectoryRefused)
	}

	request, err := peerwire.EncodeRequest(SessionDirectoryGetOp, []string{sessionID})
	if err != nil {
		return "", fmt.Errorf("%w: %v", ErrDirectoryRefused, err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), d.deadline)
	defer cancel()

	reply, err := d.caller.Call(ctx, peerwire.EventKind(SessionDirectoryPrincipalRef, SessionDirectoryStage),
		SessionDirectoryStage, 0, d.deadline, request)
	if err != nil {
		// A TRANSPORT failure, which includes the session family reporting an unreachable store
		// as ModuleStatusInternal. Retryable, and specifically not absence.
		return "", fmt.Errorf("%w: %v", peer.ErrDirectoryUnavailable, err)
	}

	// Raw, because this is the session family's status enum, not the peer family's.
	status, cells, err := peerwire.DecodeReply(reply)
	if err != nil {
		return "", fmt.Errorf("%w: undecodable reply: %v", peer.ErrDirectoryUnavailable, err)
	}

	switch status {
	case sessionStatusOK:
		if len(cells) != sessionReplyWidth {
			// A well-formed frame of the wrong shape is not an answer. Reading
			// a principal out of it would be reading whatever happened to be at
			// index 2.
			return "", fmt.Errorf("%w: reply had %d cells, want %d",
				peer.ErrDirectoryUnavailable, len(cells), sessionReplyWidth)
		}
		owner := cells[sessionReplyPrincipal]
		if owner == "" {
			// A stored session with no principal cannot be addressed, and it
			// is not absent either. Saying "no peer" would invite a caller to
			// stop; this is a row that needs fixing.
			return "", fmt.Errorf("%w: session %q has no principal",
				peer.ErrDirectoryUnavailable, sessionID)
		}
		return owner, nil

	case sessionStatusMissing:
		// The one outcome a caller may act on as final.
		return "", peer.ErrNoPeer

	case sessionStatusInvalid, sessionStatusTooLong:
		// The session family understood but refused it. That is this caller's bug.
		return "", fmt.Errorf("%w: session-family status %d", ErrDirectoryRefused, status)

	case sessionStatusFailed:
		return "", fmt.Errorf("%w: session family reported a store failure", peer.ErrDirectoryUnavailable)

	default:
		// An unrecognised status is not assumed benign and is not assumed
		// absence. Treating an unknown word as "missing" is how a store that
		// grows a fifth outcome starts reporting live sessions as gone.
		return "", fmt.Errorf("%w: unrecognised session-family status %d",
			peer.ErrDirectoryUnavailable, status)
	}
}
