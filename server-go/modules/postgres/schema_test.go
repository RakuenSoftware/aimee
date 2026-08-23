package postgres

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// fakeSchemaStore records what was executed and answers recorded versions from
// a map, so the ordering, checksum and transaction rules can be driven without
// a database.
type fakeSchemaStore struct {
	recorded map[string]string // "owner/version" -> checksum
	executed []string
	failOn   string
	inTx     bool
	rolled   bool
}

type fakeSchemaRow struct {
	values []any
	err    error
}

func (r fakeSchemaRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	for i := range dest {
		switch target := dest[i].(type) {
		case *string:
			*target = r.values[i].(string)
		case *int64:
			*target = r.values[i].(int64)
		}
	}
	return nil
}

func (s *fakeSchemaStore) Exec(ctx context.Context, sql string, args ...any) error {
	if s.failOn != "" && strings.Contains(sql, s.failOn) {
		return errors.New("statement failed")
	}
	s.executed = append(s.executed, sql)
	if strings.HasPrefix(sql, "INSERT INTO aimee_schema_version") {
		s.recorded[key(args[0].(string), args[1].(int64))] = args[2].(string)
	}
	return nil
}

func (s *fakeSchemaStore) QueryRow(ctx context.Context, sql string, args ...any) SchemaRow {
	if strings.Contains(sql, "SELECT checksum") {
		if sum, ok := s.recorded[key(args[0].(string), args[1].(int64))]; ok {
			return fakeSchemaRow{values: []any{sum}}
		}
		return fakeSchemaRow{err: ErrNoSchemaRows}
	}
	var highest int64
	for recordedKey := range s.recorded {
		var owner string
		var version int64
		if _, err := parseKey(recordedKey, &owner, &version); err == nil &&
			owner == args[0].(string) && version > highest {
			highest = version
		}
	}
	return fakeSchemaRow{values: []any{highest}}
}

func (s *fakeSchemaStore) InTx(ctx context.Context, fn func(SchemaStore) error) error {
	s.inTx = true
	before := len(s.executed)
	beforeRecorded := len(s.recorded)
	if err := fn(s); err != nil {
		// A rollback is what makes "all statements and the recorded row land
		// together" true, so the fake models it rather than pretending.
		s.executed = s.executed[:before]
		if len(s.recorded) != beforeRecorded {
			s.rolled = true
		}
		return err
	}
	return nil
}

func key(owner string, version int64) string {
	return owner + "/" + string(rune('0'+version))
}

func parseKey(k string, owner *string, version *int64) (bool, error) {
	slash := strings.LastIndex(k, "/")
	if slash < 0 {
		return false, errors.New("bad key")
	}
	*owner = k[:slash]
	*version = int64(k[slash+1] - '0')
	return true, nil
}

func newStore() *fakeSchemaStore {
	return &fakeSchemaStore{recorded: map[string]string{}}
}

func TestMigrationAppliesStatementsAndRecordsTheVersion(t *testing.T) {
	store := newStore()
	outcome, err := Migrate(context.Background(), store, Migration{
		Owner: "db1", Version: 1,
		Statements: []string{"CREATE TABLE a (id BIGINT)", "CREATE INDEX ON a (id)"},
	})
	if err != nil || outcome != MigrateApplied {
		t.Fatalf("outcome = %v, err = %v", outcome, err)
	}
	if !store.inTx {
		t.Error("a migration that is not in a transaction can be recorded half-applied")
	}
	var sawInsert bool
	for _, statement := range store.executed {
		if strings.HasPrefix(statement, "INSERT INTO aimee_schema_version") {
			sawInsert = true
		}
	}
	if !sawInsert {
		t.Error("the version was not recorded, so this migration runs again forever")
	}
}

func TestMigrationAlreadyAppliedIsNotReapplied(t *testing.T) {
	// The restart case. A module that re-ran every migration on every start
	// would reshape the corpus each time it came up.
	migration := Migration{Owner: "db2", Version: 1,
		Statements: []string{"CREATE TABLE b (id BIGINT)"}}
	store := newStore()
	store.recorded[key("db2", 1)] = migration.Checksum()

	outcome, err := Migrate(context.Background(), store, migration)
	if err != nil || outcome != MigrateAlreadyApplied {
		t.Fatalf("outcome = %v, err = %v", outcome, err)
	}
	for _, statement := range store.executed {
		if strings.HasPrefix(statement, "CREATE TABLE b") {
			t.Error("the statements ran again")
		}
	}
}

func TestMigrationEditedAfterItRanIsRefused(t *testing.T) {
	// The failure nothing currently detects: the file changed after it was
	// applied, so the source and the database disagree about what the database
	// contains. Skipping it silently keeps them disagreeing forever.
	store := newStore()
	store.recorded[key("db2", 1)] = "a-checksum-from-the-version-that-actually-ran"

	_, err := Migrate(context.Background(), store, Migration{
		Owner: "db2", Version: 1,
		Statements: []string{"CREATE TABLE b (id BIGINT, extra TEXT)"},
	})
	if !errors.Is(err, ErrMigrationChecksum) {
		t.Fatalf("err = %v, want a checksum refusal", err)
	}
}

func TestMigrationRefusesAGapRatherThanSkippingIt(t *testing.T) {
	// Applying 3 to a database that never got 2 produces a schema no version
	// number describes.
	store := newStore()
	store.recorded[key("db1", 1)] = "whatever"

	_, err := Migrate(context.Background(), store, Migration{
		Owner: "db1", Version: 3, Statements: []string{"SELECT 1"},
	})
	if !errors.Is(err, ErrMigrationGap) {
		t.Fatalf("err = %v, want a gap refusal", err)
	}
}

func TestOwnersAdvanceIndependently(t *testing.T) {
	// db1 at 1 and db2 at 1 are separate histories; db2's version 1 is not
	// blocked by, and does not satisfy, db1's.
	store := newStore()
	store.recorded[key("db1", 1)] = "db1-v1"

	outcome, err := Migrate(context.Background(), store, Migration{
		Owner: "db2", Version: 1, Statements: []string{"CREATE TABLE c (id BIGINT)"},
	})
	if err != nil || outcome != MigrateApplied {
		t.Fatalf("db2 version 1 was refused because db1 had one: %v", err)
	}
}

func TestAFailedStatementRecordsNothing(t *testing.T) {
	// The rule the whole design rests on: a migration that failed halfway must
	// not be recorded, or it can never be re-run and never be trusted.
	store := newStore()
	store.failOn = "CREATE TABLE d"

	_, err := Migrate(context.Background(), store, Migration{
		Owner: "db1", Version: 1,
		Statements: []string{"CREATE TABLE d (id BIGINT)"},
	})
	if err == nil {
		t.Fatal("a failing statement reported success")
	}
	if _, recorded := store.recorded[key("db1", 1)]; recorded {
		t.Error("a failed migration was recorded as applied")
	}
}

func TestTheAdvisoryLockIsTakenBeforeAnythingIsRead(t *testing.T) {
	// Two processes starting together both read "no version recorded" and both
	// apply, unless the lock is held across the read as well as the write.
	store := newStore()
	if _, err := Migrate(context.Background(), store, Migration{
		Owner: "db1", Version: 1, Statements: []string{"SELECT 1"},
	}); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	lock, create := -1, -1
	for index, statement := range store.executed {
		if strings.Contains(statement, "pg_advisory_xact_lock") && lock < 0 {
			lock = index
		}
		if strings.Contains(statement, "SELECT 1") {
			create = index
		}
	}
	if lock < 0 {
		t.Fatal("no advisory lock was taken; two starting processes race")
	}
	if create >= 0 && lock > create {
		t.Error("the lock was taken after the work it guards")
	}
}

func TestOwnerAndVersionAreValidated(t *testing.T) {
	store := newStore()
	for _, bad := range []Migration{
		{Owner: "", Version: 1, Statements: []string{"SELECT 1"}},
		{Owner: "DB1", Version: 1, Statements: []string{"SELECT 1"}},
		{Owner: "db1; DROP TABLE x", Version: 1, Statements: []string{"SELECT 1"}},
		{Owner: "db1", Version: 0, Statements: []string{"SELECT 1"}},
		{Owner: "db1", Version: 1},
		{Owner: "db1", Version: 1, Statements: []string{"   "}},
	} {
		if _, err := Migrate(context.Background(), store, bad); err == nil {
			t.Errorf("accepted %+v", bad)
		}
	}
}

func TestChecksumDistinguishesStatementBoundaries(t *testing.T) {
	// ["a;b"] and ["a","b"] apply differently and must not share a checksum,
	// or an edit that merges two statements reads as no change at all.
	joined := Migration{Owner: "db1", Version: 1, Statements: []string{"a;b"}}
	split := Migration{Owner: "db1", Version: 1, Statements: []string{"a", "b"}}
	if joined.Checksum() == split.Checksum() {
		t.Error("statement boundaries do not affect the checksum")
	}
}

func TestOwnerLockKeysDifferPerOwner(t *testing.T) {
	if ownerLockKey("db1") == ownerLockKey("db2") {
		t.Error("db1 and db2 serialise against each other for no reason")
	}
	if ownerLockKey("db1") < 0 || ownerLockKey("db2") < 0 {
		t.Error("a negative advisory key reads as a bug in pg_locks")
	}
}
