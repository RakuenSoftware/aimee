package controlplane

import (
	"context"
	"errors"
	"sync"

	storage "github.com/JBailes/aimee/server-go/postgres"
)

// The first thing in this tree that reaches PostgreSQL through the postgres
// module in a running deployment.
//
// The module was built, registered, declared for both placements and proven at
// parity, and nothing consumed it: the deployed db2 is the C build, and the Go
// db2 module -- which does reach PostgreSQL this way -- is not the deployed db2.
// So the architecture was true of a test path and false of the product, and a
// commit message said otherwise.
//
// This module is a Go process the image installs and the supervisor starts, so
// what it does at startup happens in production. It applies its own schema
// through the storage stage and reads it back, which makes the sentence "the
// postgres module owns PostgreSQL" true of something that runs.

// SchemaOwner namespaces this module's migrations.
//
// An owner is a schema's identity and not a module's: chosen once and never
// changed, because renaming one leaves its rows under the old name and re-runs
// its whole history against a database that already has the tables.
const SchemaOwner = "control-plane"

// The module's own state, and deliberately nothing else.
//
// Not a domain table. The corpus, the sketches and the documents move here as
// batches, each leaving db2 rather than being copied, and inventing their tables
// ahead of the operations that use them would be inventing a schema nobody
// reads. What this needs on day one is a row it can write and read back, so the
// health stage can answer whether storage works with evidence rather than with
// a guess.
var migrations = []struct {
	Version    uint64
	Statements []string
}{{
	Version: 1,
	Statements: []string{
		`CREATE TABLE IF NOT EXISTS control_plane_state (
		   id BIGINT PRIMARY KEY,
		   started_at TEXT NOT NULL,
		   module_version TEXT NOT NULL
		 )`,
	},
}}

// Store is what this module needs from storage, which is very little.
//
// An interface rather than the client, so the tests can drive it without a
// database and so this module never holds a driver. It opens no pool: the
// postgres module owns the connections and this owns the meaning.
type Store interface {
	Migrate(ctx context.Context, owner string, version uint64, checksum string,
		statements []string) error
	CurrentVersion(ctx context.Context, owner string) (uint64, string, error)
	Exec(ctx context.Context, statementID string, handle uint64, sql string,
		args ...storage.Value) (uint64, error)
}

var (
	stateMu sync.Mutex
	store   Store
	// Latched once the schema is known to be applied. A migration is idempotent
	// but a round trip per health call is not free, and the health stage is
	// called often.
	schemaReady bool
	// Why the last attempt failed, for the log rather than for the wire. A
	// failure reason on the wire would be this module explaining PostgreSQL,
	// which is the postgres module's job.
	lastErr error
)

// UseStore binds the storage this module reaches PostgreSQL through.
//
// Called once from the process entry point. A nil store leaves the module
// serving health with storage reported unreachable, which is the truthful answer
// for a deployment where the postgres module is turned off -- and it IS
// turnable-off, since postgres is in the KB's optional module list.
func UseStore(bound Store) {
	stateMu.Lock()
	defer stateMu.Unlock()
	store = bound
	schemaReady = false
	lastErr = nil
}

// ErrNoStore reports that nothing was bound.
var ErrNoStore = errors.New("control-plane: no storage is bound")

// ensureSchema applies this module's migrations and returns the recorded head.
//
// Idempotent by construction: the migration engine records (owner, version) and
// answers already-applied on a second run, comparing the checksum of the
// statements that actually arrived rather than one the caller supplied.
func ensureSchema(ctx context.Context) (uint64, error) {
	stateMu.Lock()
	bound, ready := store, schemaReady
	stateMu.Unlock()
	if bound == nil {
		return 0, ErrNoStore
	}
	if !ready {
		for _, migration := range migrations {
			// The checksum is computed over the statements being sent, and the
			// module recomputes it over what it actually received and refuses a
			// disagreement -- so neither side can record a hash of anything but
			// what ran.
			if err := bound.Migrate(ctx, SchemaOwner, migration.Version,
				storage.Checksum(migration.Statements), migration.Statements); err != nil {
				stateMu.Lock()
				lastErr = err
				stateMu.Unlock()
				return 0, err
			}
		}
	}
	version, _, err := bound.CurrentVersion(ctx, SchemaOwner)
	if err != nil {
		stateMu.Lock()
		lastErr = err
		stateMu.Unlock()
		return 0, err
	}
	stateMu.Lock()
	schemaReady = true
	lastErr = nil
	stateMu.Unlock()
	return version, nil
}

// RecordStart writes the row the health stage reads back.
//
// The write and the read are separate calls on purpose: a health stage that
// answered from what it had just written in memory would be reporting on itself.
func RecordStart(ctx context.Context, startedAt, moduleVersion string) error {
	stateMu.Lock()
	bound := store
	stateMu.Unlock()
	if bound == nil {
		return ErrNoStore
	}
	if _, err := ensureSchema(ctx); err != nil {
		return err
	}
	_, err := bound.Exec(ctx, "control_plane_record_start", 0,
		`INSERT INTO control_plane_state (id, started_at, module_version)
		 VALUES (1, $1, $2)
		 ON CONFLICT (id) DO UPDATE SET started_at = EXCLUDED.started_at,
		                                module_version = EXCLUDED.module_version`,
		storage.Text(startedAt), storage.Text(moduleVersion))
	return err
}

// storageEvidence is what the health stage can say about storage with evidence.
func storageEvidence(ctx context.Context) (reachable bool, version uint64) {
	recorded, err := ensureSchema(ctx)
	if err != nil {
		return false, 0
	}
	return true, recorded
}
