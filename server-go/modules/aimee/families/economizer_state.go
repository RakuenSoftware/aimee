// The economizer state family (event kind 11777), served from PostgreSQL.
//
// Not a "// Package ..." comment: jti_replay.go already carries this package's
// doc, and a second one makes which text godoc shows depend on file order.
//
// This is the first family of the store's move off C+SQLite. It is not a translation
// of the C module and there is no compatibility shim: the SQL here is written
// for PostgreSQL, replacing the C store's economizer_state.c (deleted with that
// module; in git history if the original is wanted)
// rather than what it defers to.
//
// The wire is unchanged, because the wire is the contract. Callers speak
// server-go/aimee's frames today and must keep speaking exactly those bytes
// across the swap -- kind 11777 is bound to ONE serving slot per bus host, so
// this module and the C one are never both answering it, and the cutover is a
// question of which process holds the slot.
//
// Two behaviours are deliberately not carried over:
//
//   - The C wrote with DELETE-then-INSERT to keep one row per session. Here the
//     primary key enforces that and the write is a single upsert: atomic, with
//     no window where a concurrent reader sees the state missing.
//   - The C read ORDER BY id DESC LIMIT 1 to pick the newest of rows that were
//     not supposed to exist. With the key enforced there is at most one row, so
//     the read is a lookup and the ordering question does not arise.
package families

import (
	"context"
	"encoding/binary"
	"errors"
	"log"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const (
	// EventState and StageState mirror server-go/aimee: kind 30 carved as
	// 4096 + ref*256 + stage. The client side is the peer of these bytes.
	EventState uint32 = 11777
	StageState uint32 = 1

	opStateLoad uint32 = 1
	opStateSave uint32 = 2

	// StateMax and KeyMax mirror the caller-side caps. They are re-checked here
	// because a module may not trust a frame just because the shipped client
	// would not have sent it.
	StateMax = 6144
	KeyMax   = 511

	statusOK      uint32 = 0
	statusMissing uint32 = 1
	statusInvalid uint32 = 2
	statusTooLong uint32 = 3
	statusFailed  uint32 = 4

	queryTimeout = 2 * time.Second
)

// loadSQL and saveSQL are the whole of this family's PostgreSQL surface.
const (
	loadSQL = `SELECT state FROM economizer_state WHERE session_id = $1`

	saveSQL = `INSERT INTO economizer_state (session_id, state)
	                VALUES ($1, $2)
	           ON CONFLICT (session_id)
	           DO UPDATE SET state = EXCLUDED.state, updated_at = now()`
)

// blobStore is the persistence this family needs, as an interface so the handler's
// wire behaviour is testable without a database.
type blobStore interface {
	load(ctx context.Context, key string) (string, bool, error)
	save(ctx context.Context, key, blob string) error
}

// request is one decoded db1-keyed-blob-v1 frame.
type request struct {
	op      uint32
	key     string
	payload string
}

// decodeRequest refuses any frame whose declared lengths do not match what
// arrived. A short read here would otherwise become an out-of-range slice.
func decodeRequest(frame []byte) (request, bool) {
	if len(frame) < 12 {
		return request{}, false
	}
	op := binary.LittleEndian.Uint32(frame[0:4])
	keyLen := binary.LittleEndian.Uint32(frame[4:8])
	if uint64(len(frame)) < uint64(8)+uint64(keyLen)+4 {
		return request{}, false
	}
	key := string(frame[8 : 8+keyLen])
	rest := frame[8+keyLen:]
	payloadLen := binary.LittleEndian.Uint32(rest[0:4])
	if uint64(len(rest)-4) != uint64(payloadLen) {
		return request{}, false
	}
	return request{op: op, key: key, payload: string(rest[4:])}, true
}

// reply builds the status/length/payload frame the client's decode expects.
func reply(status uint32, payload string) []byte {
	frame := make([]byte, 8+len(payload))
	binary.LittleEndian.PutUint32(frame[0:4], status)
	binary.LittleEndian.PutUint32(frame[4:8], uint32(len(payload)))
	copy(frame[8:], payload)
	return frame
}

// validKey mirrors the caller-side check. An embedded NUL is refused because a
// key that means one thing to Go and another to anything C-shaped downstream is
// a key worth rejecting outright.
func validKey(key string) bool {
	return key != "" && len(key) <= KeyMax && !strings.Contains(key, "\x00")
}

// pgStore reads and writes through the module's SHARED pool.
//
// It used to open its own. That was right when this family lived in a package
// of its own; now that every family is served by one process, a second pool
// would double this module's connection count against a server whose
// max_connections is a hard ceiling.
type pgStore struct{ db store.DB }

// load reports absence as (,"" false, nil): a session's first turn has no state,
// which is the normal case and not a failure.
func (s pgStore) load(ctx context.Context, key string) (string, bool, error) {
	if s.db == nil {
		return "", false, errors.New("aimee: economizer state has no store")
	}
	db := s.db
	var state string
	switch err := db.QueryRow(ctx, loadSQL, key).Scan(&state); {
	case err == nil:
		return state, true, nil
	case store.IsNoRows(err):
		return "", false, nil
	default:
		return "", false, errors.New("aimee: state load failed")
	}
}

func (s pgStore) save(ctx context.Context, key, blob string) error {
	if s.db == nil {
		return errors.New("aimee: economizer state has no store")
	}
	if _, err := s.db.Exec(ctx, saveSQL, key, blob); err != nil {
		return errors.New("aimee: state save failed")
	}
	return nil
}

// newHandler builds the stage around a store, so the wire behaviour can be
// tested against an in-memory one.
func newHandler(s blobStore) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != StageState {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if s == nil {
			return nil, bus.ModuleStatusInternal
		}
		decoded, ok := decodeRequest(frame)
		if !ok {
			return nil, bus.ModuleStatusInvalidRequest
		}
		// A refusal the module can state in-band is reported in-band. Only a
		// malformed FRAME is a bus-level invalid request: a bad key is a
		// well-formed question with an answer.
		if !validKey(decoded.key) {
			return reply(statusInvalid, ""), bus.ModuleStatusOK
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		timeout := invocation.Remaining(queryTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()

		switch decoded.op {
		case opStateLoad:
			state, found, err := s.load(ctx, decoded.key)
			if invocation.Cancelled() {
				return nil, bus.ModuleStatusCancelled
			}
			if err != nil {
				log.Printf("aimee: load failed: %v", err)
				return reply(statusFailed, ""), bus.ModuleStatusOK
			}
			if !found {
				return reply(statusMissing, ""), bus.ModuleStatusOK
			}
			// The C refused a row too big for the caller's buffer rather than
			// returning a truncated prefix, because truncated JSON does not
			// parse and a caller treating that as "no state" would silently
			// lose the conversation's page table. Same answer here, for the
			// same reason -- the cap is now the wire's rather than a buffer's.
			if len(state) >= StateMax {
				return reply(statusTooLong, ""), bus.ModuleStatusOK
			}
			return reply(statusOK, state), bus.ModuleStatusOK

		case opStateSave:
			if len(decoded.payload) >= StateMax {
				return reply(statusTooLong, ""), bus.ModuleStatusOK
			}
			if err := s.save(ctx, decoded.key, decoded.payload); err != nil {
				log.Printf("aimee: save failed: %v", err)
				return reply(statusFailed, ""), bus.ModuleStatusOK
			}
			if invocation.Cancelled() {
				return nil, bus.ModuleStatusCancelled
			}
			return reply(statusOK, ""), bus.ModuleStatusOK

		default:
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
}

// Handle is the production entry point for kind 11777, stage 1.
func Handle(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
	// Kept for the tests that drive the wire directly. The served handler is
	// built by Binds with the store it was given.
	return newHandler(pgStore{})(invocation, frame)
}

// Close is a no-op: the pool belongs to the module, not to this family, and is
// released by the module's own shutdown.
func Close() {}
