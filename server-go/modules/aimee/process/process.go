// Package process assembles the same schema and peer capabilities for bundled
// and independently exported aimee processes.
package process

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	database "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// storePrincipalRef is the store module's OUTBOUND identity, used to call the
// postgres module. Separate from its serving ref (30) because a serving grant
// requests nothing -- the rule that stops a module's right to answer becoming a
// right to ask.
//
// 69, having been 67 and then 68 in turn. Both were taken by the session
// building peer messaging -- 67 for its directory client below, 68 for the
// server's own peer client -- and it landed first. Two clients on one ref are
// two callers the bus cannot tell apart, and the failure surfaces long after
// the merge that caused it rather than at it, so this yields rather than
// contests. Declared as aimee-postgres in src/modules/process-contracts.json.
const storePrincipalRef uint32 = 69

// aimeeDirectoryPrincipalRef is the aimee module's OUTBOUND identity, used only
// to read the session directory out of aimeecontract. Same reason as the economizer's: a
// serving grant requests nothing, so reaching another module's stage needs a
// second principal granted exactly that request.
//
// 67, and deliberately not the 69 the validation probe uses. Two clients sharing
// a ref are a duplicate principal and the bus refuses whichever attaches second,
// which would surface as a failure in whichever of the two started later rather
// than at the cause.
const aimeeDirectoryPrincipalRef uint32 = 67

// aimeeDirectory builds the peer capability's DirectorySource.
//
// The choice is EXPLICIT and defaults to none. AIMEE_PEER_DIRECTORY=db1 reads
// existence from db1's session family over the bus; anything else, including
// unset, declares that there is no directory and the session-scoped stages
// answer no_directory.
//
// Defaulting to none is not caution, it is accuracy. db1's server_session_get
// returns 0 only on SQLITE_ROW and -1 for everything else including no row, and
// its stage maps a non-zero rc to FAILED, so on the store that ships today an
// absent session and a broken store are one status. Under that contract this
// module would have to report either "gone" for a transient outage -- which
// destroys mail under the undeliverable rule -- or "retry" for a session that
// will never exist. The Go store distinguishes them and the catalog now declares
// all four, so this becomes the default when db1 runs it.
func aimeeDirectory(ctx context.Context, moduleBusSocket string) (aimee.DirectorySource, string) {
	if os.Getenv("AIMEE_PEER_DIRECTORY") != "db1" {
		return aimee.NoDirectory{}, "none (set AIMEE_PEER_DIRECTORY=db1 to read db1's session family)"
	}
	if ctx == nil || moduleBusSocket == "" {
		return aimee.NoDirectory{}, "none: db1 was asked for but there is no module bus socket"
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, aimeeDirectoryPrincipalRef)
	if err != nil {
		// Reported, not silently downgraded: a module that was told to use db1
		// and quietly did not would answer no_directory for a reason nobody
		// could see.
		return aimee.NoDirectory{}, fmt.Sprintf("none: could not attach as principal %d: %v",
			aimeeDirectoryPrincipalRef, err)
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		return aimee.NoDirectory{}, fmt.Sprintf("none: no module caller: %v", err)
	}
	directory, err := aimee.NewSessionDirectory(caller, 5*time.Second)
	if err != nil {
		// CloseAndWait BEFORE Detach. The caller's goroutine is polling the
		// shared-memory region by now and Detach unmaps it; the Detach above is
		// safe only because the constructor failed and no goroutine exists yet.
		// Same defect that segfaulted the probe after every check passed.
		caller.CloseAndWait()
		busClient.Detach()
		return aimee.NoDirectory{}, fmt.Sprintf("none: %v", err)
	}
	return directory, fmt.Sprintf("db1 sessions (kind %d) as principal 1/%d",
		peerwire.EventKind(aimee.SessionDirectoryPrincipalRef, aimee.SessionDirectoryStage), aimeeDirectoryPrincipalRef)
}

// storeBackend is db1's storage: the postgres module, over the bus.
//
// db1 opens no database. The postgres module owns the connection, the DSN and
// the pooling policy, and this is the client that asks it -- the same shape as
// economizerStore above, under db1's outbound identity.
// applySchemaWaiting applies the schema, giving the postgres module time to
// finish attaching first.
//
// THE SUPERVISOR GIVES NO ORDERING GUARANTEE. It starts every module in its
// manifest back to back and each registers its stages asynchronously, so the
// store routinely makes its first call before postgres has claimed kind 11266.
// Without this the store exits at startup and the daemon comes up storeless:
//
//	[module-supervisor:server] starting postgres
//	store: schema: read the applied schema version: ... capability absent
//	aimee-module: module "aimee" could not start
//
// Observed on a sixteen-module fleet, where the two start one line apart.
// Ordering the manifest would not fix it -- registration is asynchronous, so
// starting postgres first only narrows the window, and a window that closes on
// a fast machine reopens on a loaded one.
//
// A BOUNDED WAIT. If postgres is genuinely absent -- not installed, refused by
// its grant, unable to open the database -- the store must still fail and say
// so rather than hang forever looking healthy. The ceiling is what separates
// "not up yet" from "not coming".
//
// Retrying ONLY ErrStoreUnavailable, which is the transport reporting it could
// not reach the module at all. A store that answers and refuses is a different
// thing, and retrying that would turn one clear error into the same error
// thirty times.
func applySchemaWaiting(ctx context.Context, db aimee.Store) error {
	const (
		attempts = 30
		gap      = time.Second
	)
	var err error
	for attempt := 1; attempt <= attempts; attempt++ {
		err = families.ApplySchema(ctx, db)
		if err == nil {
			if attempt > 1 {
				log.Printf("store: the postgres module answered after %ds", attempt-1)
			}
			return nil
		}
		if !errors.Is(err, database.ErrStoreUnavailable) {
			return err
		}
		if attempt == attempts {
			break
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(gap):
		}
	}
	return fmt.Errorf("the postgres module did not answer within %ds: %w", attempts, err)
}

func storeBackend(ctx context.Context, moduleBusSocket string) (database.Store, error) {
	if ctx == nil || moduleBusSocket == "" {
		return nil, errors.New("store: no module bus to reach the postgres module on")
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, storePrincipalRef)
	if err != nil {
		return nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		return nil, err
	}
	db, err := database.NewStore(caller)
	if err != nil {
		caller.CloseAndWait()
		busClient.Detach()
		return nil, err
	}
	return db, nil
}

// New initializes storage and applies the complete schema before advertising
// any capability. Both executable forms use this startup path.
func New(ctx context.Context, moduleBusSocket string) (*aimee.Module, error) {
	// Peer messaging. The registry is process-local: inboxes and grants
	// live in memory and do not survive a bounce.
	//
	// Its DirectorySource is the store's session family, which is why the
	// store is REQUIRED below rather than optional. Peer messaging with no
	// directory is not degraded, it is inert -- every session-scoped call
	// refuses with unknown_sender or no_peer, which are answers ABOUT A
	// SESSION from a module that cannot know about any session. Serving
	// four peer stages out of this principal's twenty-three while the store
	// is unreachable would advertise exactly that: a correct-looking
	// refusal from something that can never do anything.
	directory, sourceDescription := aimeeDirectory(ctx, moduleBusSocket)
	peerCapability, err := aimee.NewPeer(peer.New(peer.Options{}), directory)
	if err != nil {
		log.Printf("aimee module unavailable: %v", err)
		return nil, err
	}
	// Logged at every start, not once at build time: an operator reading
	// why a peer send refuses should find the reason in the log of the
	// process that refused it. It names the source either way, so a run
	// that MEANT to use the session family and did not is visible rather
	// than looking the same as one that never asked.
	log.Printf("aimee module: session directory = %s", sourceDescription)

	db, err := storeBackend(ctx, moduleBusSocket)
	if err != nil {
		// Without a store this module serves nothing. Declaring its stages
		// anyway would have the daemon route every store call here to fail
		// one at a time; declaring none makes it report the kinds as
		// unserved, which is what is true.
		log.Printf("store: no store backend: %v", err)
		return nil, err
	}
	// Create anything missing before serving. Nothing else applies this
	// schema -- there is no deploy step for it -- so a fresh database would
	// otherwise come up empty and fail every call against tables that were
	// never created.
	schemaCtx, cancelSchema := context.WithTimeout(context.Background(), 2*time.Minute)
	err = applySchemaWaiting(schemaCtx, db)
	cancelSchema()
	if err != nil {
		log.Printf("store: schema: %v", err)
		return nil, err
	}
	log.Printf("store: schema applied (%d files)", families.SchemaFileCount())
	mux, err := aimee.NewMux(db, families.All()...)
	if err != nil {
		log.Printf("store: %v", err)
		return nil, err
	}
	for _, bind := range families.Binds(db) {
		if err := mux.Add(bind); err != nil {
			log.Printf("store: %v", err)
			return nil, err
		}
	}
	// One stage table, one handler. Mux answers stages 1..19 and the peer
	// capability 20..23; New refuses a collision at construction rather
	// than letting the loser be silently unreachable.
	module, err := aimee.New(peerCapability, mux)
	if err != nil {
		log.Printf("aimee module unavailable: %v", err)
		return nil, err
	}
	return module, nil
}
