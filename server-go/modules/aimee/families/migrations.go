package families

import (
	"fmt"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// aimee's schema as a versioned history, not a pile of idempotent files.
//
// What this replaces: nineteen CREATE TABLE IF NOT EXISTS files re-applied in
// full on every start. That works for a fresh database and does nothing useful
// afterwards -- there is no record of what ran, no way to tell a database that
// is current from one that is behind, and no way to detect a file edited after
// it was applied. The store the C module left behind had the same shape, plus
// loose migration files an operator was told to run by hand.
//
// The store module records (owner, version, checksum) and refuses a version
// whose checksum has changed since it ran, a version applied out of order, and
// a partially applied migration. aimee owns the content; the module owns only the
// fact that it ran. Namespaced by owner because aimee's and db2's histories are
// independent -- aimee at 19 and db2 at 31 is a normal state.
//
// VERSIONS ARE EXPLICIT, NOT DERIVED. Numbering them by sorted filename would
// be stable only until someone adds a family whose name sorts in the middle,
// at which point every later version renumbers and every checksum stops
// matching a recorded row -- the store would read that as nineteen files edited
// after they ran and refuse to start. So the order lives here, and a new
// migration is a line appended to the end.

// SchemaOwner names aimee's schema history in the store's version table. Its own
// namespace: db2 keeps a separate one, and the two move independently.
//
// STILL "db1" AFTER THE MODULE BECAME "aimee", deliberately. This string is not
// a label, it is the key the store has recorded against every migration it has
// applied, on every database that has ever run this module. Changing it does not
// rename those rows -- it makes them invisible, so the store reports the schema
// as version 0, and the next start tries to apply all twenty-one migrations to a
// database that already has them. The first CREATE TABLE would fail and the
// module would not come up.
//
// So the owner name is a value in persisted state, not a name in source, and it
// can only change alongside a migration that rewrites the rows. The error
// messages below say "db1 schema" for the same reason: they name the history,
// which is what a reader would have to go looking for.
const SchemaOwner = "db1"

// schemaHistory is aimee's migrations in application order. Append only: an
// existing line's version and file must never change, because the store has
// recorded that pairing on every database that has run it.
//
// The initial nineteen are the families' schemas as they stood when aimee became
// a module. They are independent of each other -- no cross-family foreign keys,
// which is what lets the per-family test suites apply one file alone -- so
// their relative order among themselves is arbitrary and fixed here only so it
// can never change again.
var schemaHistory = []struct {
	Version int64
	File    string
}{
	{1, "schema_agent_work.sql"},
	{2, "schema_checkpoints.sql"},
	{3, "schema_conversation.sql"},
	{4, "schema_delegation.sql"},
	{5, "schema_economizer.sql"},
	{6, "schema_ensemble.sql"},
	{7, "schema_git.sql"},
	{8, "schema_guardrail.sql"},
	{9, "schema_identity.sql"},
	{10, "schema_jti.sql"},
	{11, "schema_jwks.sql"},
	{12, "schema_lifecycle.sql"},
	{13, "schema_nonce.sql"},
	{14, "schema_pki.sql"},
	{15, "schema_roundtable.sql"},
	{16, "schema_runtime.sql"},
	{17, "schema_sessions.sql"},
	{18, "schema_telemetry.sql"},
	{19, "schema_workflow.sql"},
	// Appended after the initial nineteen: the first migration that is a change
	// rather than a starting point. It caps the twelve columns whose values are
	// read back through 1 MiB catalog fields, so a value the store wire would
	// refuse on the way out is refused on the way in instead.
	{20, "schema_cell_limits.sql"},
	// The same defect from the other side: columns that DO carry a limit, but
	// count it in characters while every consumer counts bytes.
	{21, "schema_length_units.sql"},
	// Upstream kept developing the C store while this branch replaced it: eval
	// candidates and approach memory landed there with no Go equivalent, and
	// src/db1_client already calls all ten of their operations.
	{22, "schema_eval_candidates.sql"},
}

// Migration is one versioned change to aimee's schema.
type Migration struct {
	Owner   string
	Version int64
	// Name is the file it came from, for a human reading a failure.
	Name string
	// Checksum is sha256 over Statements exactly as they will be applied. The
	// store compares it against what it recorded: a difference means the file
	// changed after it ran, so the database and the source disagree about what
	// the database contains.
	Checksum string
	// Statements are applied in order, in one transaction.
	//
	// One entry per file rather than one per SQL statement. Splitting these
	// safely means parsing SQL -- several carry DO $$ ... $$ blocks whose
	// bodies contain semicolons -- and a splitter that gets that wrong produces
	// fragments that fail in ways no reader could explain. PostgreSQL applies a
	// multi-statement string atomically inside a transaction, which is the
	// property that matters.
	Statements []string
}

// Migrations returns aimee's schema history, checksummed, in application order.
func Migrations() ([]Migration, error) {
	seen := map[int64]bool{}
	out := make([]Migration, 0, len(schemaHistory))
	for i, entry := range schemaHistory {
		if seen[entry.Version] {
			return nil, fmt.Errorf("db1 schema: version %d appears twice", entry.Version)
		}
		seen[entry.Version] = true
		// Gaps are refused by the store, so catch them here where the message
		// can name the file rather than arriving as a rejected migration.
		if want := int64(i + 1); entry.Version != want {
			return nil, fmt.Errorf("db1 schema: %s is version %d, expected %d "+
				"-- the history is append-only and has no gaps",
				entry.File, entry.Version, want)
		}
		body, err := schemaFS.ReadFile(entry.File)
		if err != nil {
			return nil, fmt.Errorf("db1 schema: version %d names %s, which is not "+
				"embedded: a migration's file may never be removed or renamed, "+
				"because the store recorded that pairing when it ran",
				entry.Version, entry.File)
		}
		statements := []string{string(body)}
		out = append(out, Migration{
			Owner:      SchemaOwner,
			Version:    entry.Version,
			Name:       entry.File,
			Checksum:   checksum(statements),
			Statements: statements,
		})
	}
	return out, nil
}

// checksum defers to the wire's construction: the store recomputes it, so it is
// a property of the contract rather than of how aimee loads its files.
func checksum(statements []string) string { return store.StoreChecksum(statements) }

// PendingMigrations returns the migrations above the version the store has
// recorded for aimee. A store at 19 gets nothing and starts without touching the
// schema, which is the ordinary case on every start after the first.
func PendingMigrations(applied int64) ([]Migration, error) {
	all, err := Migrations()
	if err != nil {
		return nil, err
	}
	out := make([]Migration, 0, len(all))
	for _, m := range all {
		if m.Version > applied {
			out = append(out, m)
		}
	}
	return out, nil
}
