package aimee

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// DB1Directory answers existence from db1's session family over the bus.
//
// # Why this is written but NOT wired
//
// The module still constructs NoDirectory. This type is complete and tested and
// deliberately not in the deployed path, because which db1 answers decides
// whether it can be correct, and the one that ships today cannot.
//
// The Go store distinguishes the three outcomes: an absent row answers
// StatusMissing, a broken store answers StatusFailed. Against that, the mapping
// below is exact.
//
// The C store, which is what `db1` still declares (runtime "c" in
// process-contracts.json), does not. Its db1_server_session_get returns 0 only
// on SQLITE_ROW and -1 for a bad argument, a missing connection, a prepare
// failure AND a row that is not there, and sessions_stage maps any non-zero rc
// to STATUS_FAILED. Absent and broken arrive as one status.
//
// Wiring this against the C store would degrade SAFELY -- absence reads as
// ErrDirectoryUnavailable, so a caller retries rather than concluding a session
// is gone, and no mail is destroyed. But every genuinely unknown session would
// become a retry that never terminates, and the module would answer "the
// directory is having trouble" about a session that simply does not exist. That
// is a plausible statement replacing a true one: today's no_directory is
// correct, and correct beats plausible.
//
// So this waits for the Go store to be what `db1` runs. Wiring it is then one
// line in cmd/aimee-module, and the deployment validation is re-run against it.
type DB1Directory struct {
	caller   ModuleCaller
	deadline time.Duration
}

// ModuleCaller is the narrow slice of bus.ConcurrentModuleCaller this needs.
//
// An interface rather than the concrete type so the STATUS MAPPING is testable
// without a bus. That mapping is the whole substance here, and it is the part
// that cannot be exercised on a live system until the store that produces every
// status is the one running.
type ModuleCaller interface {
	Call(ctx context.Context, eventKind, stageID uint32, traceID uint64,
		deadline time.Duration, request []byte) ([]byte, error)
}

// db1 identity and contract. Derived, not transcribed: the kind follows from the
// ref and stage by the bus formula, so it cannot drift from db1's declaration.
const (
	// DB1PrincipalRef is db1's principal. After the absorption this module and
	// db1 share it, which is what makes their stage tables meet in one place.
	DB1PrincipalRef uint32 = 30
	// DB1SessionsStage is db1's `sessions` family, id 6 in its catalog.
	DB1SessionsStage uint32 = 6
	// DB1OpServerSessionGet is AIMEE_DB1_OP_SERVER_SESSION_GET.
	DB1OpServerSessionGet uint32 = 2
	// db1ReplyPrincipal is the index of `principal` in the ten-cell reply:
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
	db1ReplyPrincipal = 2
	db1ReplyWidth     = 10
)

// db1 status words. NOT peerwire.Status: the integers collide and the meanings
// do not. db1's 1 is MISSING where this module's 1 is no_peer, and its 4 is
// FAILED where this module's 4 is hop_limit.
const (
	db1StatusOK      uint32 = 0
	db1StatusMissing uint32 = 1
	db1StatusInvalid uint32 = 2
	db1StatusTooLong uint32 = 3
	db1StatusFailed  uint32 = 4
)

// ErrDirectoryRefused is db1 refusing the REQUEST rather than failing.
//
// Separate from ErrDirectoryUnavailable because it means this module sent
// something db1 would not accept, which is a bug here and no amount of retrying
// fixes it. Reported as unavailable, it would look like a store that never
// recovers.
var ErrDirectoryRefused = errors.New("aimee: db1 refused the directory request")

// NewDB1Directory builds a directory over a bus caller.
func NewDB1Directory(caller ModuleCaller, deadline time.Duration) (*DB1Directory, error) {
	if caller == nil {
		return nil, errors.New("aimee: db1 directory needs a bus caller")
	}
	if deadline <= 0 {
		deadline = 5 * time.Second
	}
	return &DB1Directory{caller: caller, deadline: deadline}, nil
}

// Owner reports a session's owner principal, or why it cannot.
//
// The four outcomes are kept apart because the caller's correct response to each
// is different: use it, stop, fix the call, retry.
func (d *DB1Directory) Owner(sessionID string) (string, error) {
	if d == nil || d.caller == nil {
		return "", peer.ErrNoDirectory
	}
	if sessionID == "" {
		// Refused here rather than sent. db1 answers StatusInvalid for it, so
		// asking would spend a round trip to be told what is already known.
		return "", fmt.Errorf("%w: empty session id", ErrDirectoryRefused)
	}

	request, err := peerwire.EncodeRequest(DB1OpServerSessionGet, []string{sessionID})
	if err != nil {
		return "", fmt.Errorf("%w: %v", ErrDirectoryRefused, err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), d.deadline)
	defer cancel()

	reply, err := d.caller.Call(ctx, peerwire.EventKind(DB1PrincipalRef, DB1SessionsStage),
		DB1SessionsStage, 0, d.deadline, request)
	if err != nil {
		// A TRANSPORT failure, which includes db1 reporting an unreachable store
		// as ModuleStatusInternal. Retryable, and specifically not absence.
		return "", fmt.Errorf("%w: %v", peer.ErrDirectoryUnavailable, err)
	}

	// Raw, because this is db1's status enum and not this module's.
	status, cells, err := peerwire.DecodeReply(reply)
	if err != nil {
		return "", fmt.Errorf("%w: undecodable reply: %v", peer.ErrDirectoryUnavailable, err)
	}

	switch status {
	case db1StatusOK:
		if len(cells) != db1ReplyWidth {
			// A well-formed frame of the wrong shape is not an answer. Reading
			// a principal out of it would be reading whatever happened to be at
			// index 2.
			return "", fmt.Errorf("%w: reply had %d cells, want %d",
				peer.ErrDirectoryUnavailable, len(cells), db1ReplyWidth)
		}
		owner := cells[db1ReplyPrincipal]
		if owner == "" {
			// A session db1 holds with no principal cannot be addressed, and it
			// is not absent either. Saying "no peer" would invite a caller to
			// stop; this is a row that needs fixing.
			return "", fmt.Errorf("%w: session %q has no principal",
				peer.ErrDirectoryUnavailable, sessionID)
		}
		return owner, nil

	case db1StatusMissing:
		// The one outcome a caller may act on as final.
		return "", peer.ErrNoPeer

	case db1StatusInvalid, db1StatusTooLong:
		// db1 understood and would not accept it. That is this module's bug.
		return "", fmt.Errorf("%w: db1 status %d", ErrDirectoryRefused, status)

	case db1StatusFailed:
		return "", fmt.Errorf("%w: db1 reported a store failure", peer.ErrDirectoryUnavailable)

	default:
		// An unrecognised status is not assumed benign and is not assumed
		// absence. Treating an unknown word as "missing" is how a store that
		// grows a fifth outcome starts reporting live sessions as gone.
		return "", fmt.Errorf("%w: unrecognised db1 status %d",
			peer.ErrDirectoryUnavailable, status)
	}
}
