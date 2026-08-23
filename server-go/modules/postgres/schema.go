package postgres

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/fnv"
	"strings"
)

// Versioned schema migration, namespaced by the owner of the schema.
//
// DB1 and DB2 hold different schemas with independent release histories: db1 at
// version 7 while db2 is at 31 is a normal state, not a skew to reconcile. A
// single global counter would couple two cadences that have no reason to move
// together, so the version is (owner, version) and each owner advances alone.
//
// What this replaces is worth stating, because it is the reason for every rule
// below. Today db2 applies one idempotent schema.sql and then carries loose
// migration files -- 2026-embed-halfvec.sql, 2026-embed-dim-1024.sql,
// 2026-vector-diskann.sql -- each ending in a comment asking an operator to run
// it by hand and restart. Nothing records that they ran, nothing enforces
// order, and nothing notices when one is edited after the fact. db2 also needed
// its own advisory lock because two processes starting together both tried to
// reshape the corpus at once.

// SchemaVersionTable is owned by this module and written by nothing else.
//
// It is created by the migration path itself rather than by any schema file:
// the table that records migrations cannot itself be migrated by the mechanism
// it records.
const SchemaVersionTable = `CREATE TABLE IF NOT EXISTS aimee_schema_version (
 owner      TEXT   NOT NULL,
 version    BIGINT NOT NULL,
 checksum   TEXT   NOT NULL,
 applied_at TEXT   NOT NULL,
 PRIMARY KEY (owner, version)
)`

// Migration is one ordered, checksummed step in an owner's history.
type Migration struct {
	Owner      string
	Version    int64
	Statements []string
}

// Checksum is taken over the statements as applied, so an edit after the fact
// is detectable. Joined with a separator that cannot occur in the join of two
// different statement lists, so ["a;b"] and ["a","b"] do not collide.
func (m Migration) Checksum() string {
	sum := sha256.New()
	for _, statement := range m.Statements {
		fmt.Fprintf(sum, "%d\x00%s\x00", len(statement), statement)
	}
	return hex.EncodeToString(sum.Sum(nil))
}

// Errors a caller is expected to distinguish. A migration refused because it
// was edited is a different situation from one refused because an earlier
// version is missing, and both differ from a database that is simply ahead.
var (
	ErrMigrationChecksum = errors.New("postgres: recorded migration has a different checksum")
	ErrMigrationGap      = errors.New("postgres: an earlier migration version is missing")
	ErrMigrationOwner    = errors.New("postgres: migration owner is empty or malformed")
	ErrMigrationVersion  = errors.New("postgres: migration version must be positive")
	ErrMigrationEmpty    = errors.New("postgres: migration carries no statements")
)

// MigrateOutcome says what happened, because "applied" and "already applied"
// are both success and a caller restarting needs to tell them apart.
type MigrateOutcome uint8

const (
	MigrateApplied MigrateOutcome = iota
	MigrateAlreadyApplied
)

// ownerLockKey maps an owner to a Postgres advisory lock key.
//
// Hashed rather than assigned from a table so a new owner needs no
// registration. The advisory lock is what stops two processes starting
// together from both applying the same migration -- db2 hit exactly that and
// grew a hand-rolled lock of its own.
func ownerLockKey(owner string) int64 {
	sum := fnv.New64a()
	sum.Write([]byte("aimee.schema."))
	sum.Write([]byte(owner))
	// Masked to 63 bits: the advisory lock key is signed, and a negative key
	// works but reads as a bug every time anyone sees it in pg_locks.
	return int64(sum.Sum64() & 0x7fffffffffffffff)
}

func validOwner(owner string) bool {
	if owner == "" || len(owner) > 64 {
		return false
	}
	for _, r := range owner {
		if !(r == '-' || r == '_' || (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9')) {
			return false
		}
	}
	return true
}

// SchemaStore is the narrow slice of a pool this needs, so the engine is
// testable without one.
type SchemaStore interface {
	Exec(ctx context.Context, sql string, args ...any) error
	QueryRow(ctx context.Context, sql string, args ...any) SchemaRow
	InTx(ctx context.Context, fn func(SchemaStore) error) error
}

// SchemaRow is a single-row result.
type SchemaRow interface {
	Scan(dest ...any) error
}

// ErrNoSchemaRows is returned by a SchemaRow whose query matched nothing.
var ErrNoSchemaRows = errors.New("postgres: no rows")

// CurrentVersion answers the highest version recorded for an owner, and 0 when
// the owner has no history. A caller asks this before deciding what to send,
// so the migration set can live entirely on the owner's side.
func CurrentVersion(ctx context.Context, store SchemaStore, owner string) (int64, error) {
	if !validOwner(owner) {
		return 0, ErrMigrationOwner
	}
	if err := store.Exec(ctx, SchemaVersionTable); err != nil {
		return 0, err
	}
	var version int64
	err := store.QueryRow(ctx,
		`SELECT COALESCE(MAX(version), 0) FROM aimee_schema_version WHERE owner = $1`,
		owner).Scan(&version)
	if err != nil && !errors.Is(err, ErrNoSchemaRows) {
		return 0, err
	}
	return version, nil
}

// Migrate applies one version, or reports that it is already applied.
//
// Every rule here exists because its absence leaves a database nobody can
// repair:
//
//   - the statements and the recorded row land in ONE transaction, because a
//     migration that half-ran and was recorded as complete cannot be re-run and
//     cannot be trusted;
//   - a recorded version whose checksum differs is refused rather than skipped,
//     because it means the source and the database disagree about what the
//     database contains, which is the failure you most want to hear about and
//     the one nothing currently detects;
//   - a gap is refused, because applying 5 to a database missing 4 produces a
//     schema no version number describes;
//   - the whole thing runs under an advisory lock keyed on the owner, so two
//     processes starting together serialise instead of racing.
func Migrate(ctx context.Context, store SchemaStore, migration Migration) (MigrateOutcome, error) {
	if !validOwner(migration.Owner) {
		return 0, ErrMigrationOwner
	}
	if migration.Version < 1 {
		return 0, ErrMigrationVersion
	}
	if len(migration.Statements) == 0 {
		return 0, ErrMigrationEmpty
	}
	for _, statement := range migration.Statements {
		if strings.TrimSpace(statement) == "" {
			return 0, ErrMigrationEmpty
		}
	}

	checksum := migration.Checksum()
	outcome := MigrateApplied
	err := store.InTx(ctx, func(tx SchemaStore) error {
		if err := tx.Exec(ctx, SchemaVersionTable); err != nil {
			return err
		}
		// Held until the transaction ends, so the lock and the work it guards
		// cannot come apart.
		if err := tx.Exec(ctx, `SELECT pg_advisory_xact_lock($1)`,
			ownerLockKey(migration.Owner)); err != nil {
			return err
		}

		var recorded string
		err := tx.QueryRow(ctx,
			`SELECT checksum FROM aimee_schema_version WHERE owner = $1 AND version = $2`,
			migration.Owner, migration.Version).Scan(&recorded)
		switch {
		case err == nil && recorded == checksum:
			outcome = MigrateAlreadyApplied
			return nil
		case err == nil:
			return fmt.Errorf("%w: %s version %d recorded as %s, now %s",
				ErrMigrationChecksum, migration.Owner, migration.Version,
				recorded[:min(8, len(recorded))], checksum[:8])
		case !errors.Is(err, ErrNoSchemaRows):
			return err
		}

		var highest int64
		if err := tx.QueryRow(ctx,
			`SELECT COALESCE(MAX(version), 0) FROM aimee_schema_version WHERE owner = $1`,
			migration.Owner).Scan(&highest); err != nil &&
			!errors.Is(err, ErrNoSchemaRows) {
			return err
		}
		if migration.Version != highest+1 {
			return fmt.Errorf("%w: %s is at %d, cannot apply %d",
				ErrMigrationGap, migration.Owner, highest, migration.Version)
		}

		for index, statement := range migration.Statements {
			if err := tx.Exec(ctx, statement); err != nil {
				return fmt.Errorf("%s version %d statement %d: %w",
					migration.Owner, migration.Version, index+1, err)
			}
		}
		// The stamp is written with a plain expression rather than with
		// pg_now_text(). That helper belongs to db2's schema, and this module
		// records migrations for every owner -- against a database that is not
		// db2's, calling it fails with "function pg_now_text() does not exist".
		// Same canonical UTC spelling, no dependency on anyone's schema.
		return tx.Exec(ctx,
			`INSERT INTO aimee_schema_version (owner, version, checksum, applied_at)
			 VALUES ($1, $2, $3,
			         to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC',
			                 'YYYY-MM-DD"T"HH24:MI:SS"Z"'))`,
			migration.Owner, migration.Version, checksum)
	})
	if err != nil {
		return 0, err
	}
	return outcome, nil
}
