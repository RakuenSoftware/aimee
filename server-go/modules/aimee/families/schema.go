package families

import (
	"context"
	"embed"
	"fmt"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The store's schema, carried in the binary that serves it.
//
// Nothing else applies it. The C store's schema went into the database through
// db_apply_schema at init; when the store became this module the files moved
// but no deploy step took over applying them, so a fresh install came up with
// an empty database and every call failed against a table that was never
// created. The suites apply them with psql, which is why that gap stayed
// invisible -- the only databases anyone tested against had already had the
// schema pushed to them by hand.
//
// Owning the schema is also what lets a test fixture point the module at an
// empty database and get a working one, with no psql on the box.

//go:embed schema_*.sql
var schemaFS embed.FS

// SchemaMigrator is the store's migration capability: it reports how far an
// owner's history has been applied and applies what is missing, recording each
// version with its checksum in the same transaction as the change.
//
// An interface so aimee depends on the capability rather than on a particular
// store, and so this can be exercised without one.
type SchemaMigrator interface {
	CurrentSchemaVersion(ctx context.Context, owner string) (int64, string, error)
	Migrate(ctx context.Context, m store.MigrationRequest) error
}

// ApplySchema brings aimee's schema up to date, and does nothing at all when it
// already is -- which is every start after the first.
//
// It does NOT run DDL as ordinary statements. The store applies each version
// under its own lock and records that it ran, so this is idempotent by
// bookkeeping rather than by every statement being written IF NOT EXISTS, and a
// file edited after it was applied is refused instead of silently diverging
// from what the database contains.
func ApplySchema(ctx context.Context, m SchemaMigrator) error {
	applied, _, err := m.CurrentSchemaVersion(ctx, SchemaOwner)
	if err != nil {
		return fmt.Errorf("read the applied schema version: %w", err)
	}
	pending, err := PendingMigrations(applied)
	if err != nil {
		return err
	}
	for _, migration := range pending {
		err := m.Migrate(ctx, store.MigrationRequest{
			Owner:      migration.Owner,
			Version:    migration.Version,
			Checksum:   migration.Checksum,
			Statements: migration.Statements,
		})
		if err != nil {
			// The version and the file, because the store's own message names
			// neither and "migration failed" sends the reader to the wrong file.
			return fmt.Errorf("apply %s version %d (%s): %w",
				migration.Owner, migration.Version, migration.Name, err)
		}
	}
	return nil
}

// PendingCount is how many migrations a store at this version still needs, for
// a startup line that says something checkable rather than "ok".
func PendingCount(applied int64) int {
	pending, err := PendingMigrations(applied)
	if err != nil {
		return 0
	}
	return len(pending)
}

// SchemaFileCount is how many files ApplySchema will run, for a startup log
// line that says something checkable rather than "ok".
func SchemaFileCount() int {
	entries, err := schemaFS.ReadDir(".")
	if err != nil {
		return 0
	}
	n := 0
	for _, e := range entries {
		if !e.IsDir() {
			n++
		}
	}
	return n
}
