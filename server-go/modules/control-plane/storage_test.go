package controlplane

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	modulepg "github.com/JBailes/aimee/server-go/modules/postgres"
	storage "github.com/JBailes/aimee/server-go/postgres"
)

type recordingStore struct {
	migrations []uint64
	owners     []string
	checksums  []string
	statements [][]string
	execs      []string
	version    uint64
	migrateErr error
	versionErr error
	execErr    error
}

func (s *recordingStore) Migrate(_ context.Context, owner string, version uint64,
	checksum string, statements []string) error {
	s.owners = append(s.owners, owner)
	s.migrations = append(s.migrations, version)
	s.checksums = append(s.checksums, checksum)
	s.statements = append(s.statements, statements)
	return s.migrateErr
}

func (s *recordingStore) CurrentVersion(_ context.Context, _ string) (uint64, string, error) {
	return s.version, "", s.versionErr
}

func (s *recordingStore) Exec(_ context.Context, statementID string, _ uint64, sql string,
	_ ...storage.Value) (uint64, error) {
	s.execs = append(s.execs, statementID+" "+sql)
	return 1, s.execErr
}

func healthOnce(t *testing.T) Ready {
	t.Helper()
	body, status := Handle(bus.ModuleInvocation{StageID: StageHealth}, EncodeHealthRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("health status = %v", status)
	}
	ready, err := DecodeHealthReply(body)
	if err != nil {
		t.Fatalf("decode health: %v", err)
	}
	return ready
}

func TestHealthReportsStorageOnlyWhenItReachedIt(t *testing.T) {
	// The flag is evidence rather than configuration: it is set from a migration
	// and a version read that both crossed the bus. A module that reported
	// "storage reachable" because a store had been BOUND would be reporting on
	// its own wiring, which is true of a store pointing at nothing.
	t.Cleanup(func() { UseStore(nil) })

	UseStore(nil)
	if healthOnce(t).StorageReachable {
		t.Error("storage reported reachable with nothing bound")
	}

	// Bound but failing. This is the case that separates evidence from wiring.
	UseStore(&recordingStore{migrateErr: errors.New("connection reset")})
	if healthOnce(t).StorageReachable {
		t.Error("storage reported reachable when the migration failed")
	}

	UseStore(&recordingStore{version: 1})
	if !healthOnce(t).StorageReachable {
		t.Error("storage reported unreachable when it answered")
	}
}

func TestTheSchemaIsAppliedUnderThisModulesOwnName(t *testing.T) {
	// An owner is a schema's identity, not a module's, and this one is chosen
	// once: renaming it would leave these rows under the old name and re-run the
	// whole history against a database that already has the tables.
	t.Cleanup(func() { UseStore(nil) })
	store := &recordingStore{version: 1}
	UseStore(store)

	if err := RecordStart(context.Background(), "2026-01-01T00:00:00Z", "test"); err != nil {
		t.Fatalf("record start: %v", err)
	}
	if len(store.owners) != 1 || store.owners[0] != SchemaOwner {
		t.Fatalf("migrated under %v, want %q", store.owners, SchemaOwner)
	}
	if store.migrations[0] != 1 {
		t.Errorf("version = %d, want 1", store.migrations[0])
	}
	// The checksum is over the statements actually sent. The module recomputes
	// it over what it received and refuses a disagreement, so a checksum taken
	// from anywhere else would be refused rather than recorded.
	if store.checksums[0] != storage.Checksum(store.statements[0]) {
		t.Error("the checksum does not cover the statements that were sent")
	}
	if len(store.execs) != 1 || !strings.Contains(store.execs[0], "control_plane_state") {
		t.Fatalf("execs = %v", store.execs)
	}
	// The operation names itself, so the storage module can say what a caller
	// was doing without parsing SQL.
	if !strings.HasPrefix(store.execs[0], "control_plane_record_start ") {
		t.Errorf("the statement did not name its operation: %q", store.execs[0])
	}
}

func TestNothingIsBoundMeansNothingIsWritten(t *testing.T) {
	// A module whose storage is absent must not pretend. postgres is in the KB's
	// optional module list, so this is an operator's choice rather than a fault,
	// and the honest answer is a refusal the caller can read.
	t.Cleanup(func() { UseStore(nil) })
	UseStore(nil)
	if err := RecordStart(context.Background(), "2026-01-01T00:00:00Z", "test"); !errors.Is(err, ErrNoStore) {
		t.Fatalf("err = %v, want ErrNoStore", err)
	}
}

// TestLiveTheSchemaLandsInARealDatabaseThroughTheModule is the one that makes
// the architecture a fact rather than a diagram.
//
// It binds the control-plane module's storage to the postgres module's storage
// handler and applies the schema, then reads the recorded version back and the
// row it wrote. Every call crosses the storage codec.
//
// The handler is bound directly rather than over the event bus: the bus is
// proven elsewhere and a failure through it could be either, while what is in
// question here is whether this module's schema and row survive the wire.
func TestLiveTheSchemaLandsInARealDatabaseThroughTheModule(t *testing.T) {
	if os.Getenv("AIMEE_DB2_URL") == "" {
		t.Skip("set AIMEE_DB2_URL to run the live control-plane storage suite")
	}
	t.Cleanup(func() { UseStore(nil) })

	handler := modulepg.NewSQLHandler()
	invocation := bus.ModuleInvocation{
		StageID: modulepg.StageSQL, PrincipalRef: 32, SrcHandle: 1}
	calls := 0
	client := storage.New(func(_ context.Context, body []byte) ([]byte, error) {
		calls++
		reply, status := handler(invocation, body)
		if status != bus.ModuleStatusOK {
			t.Fatalf("postgres module status = %v", status)
		}
		return reply, nil
	})
	UseStore(client)

	if err := RecordStart(context.Background(), "2026-01-01T00:00:00Z", "live-test"); err != nil {
		t.Fatalf("record start: %v", err)
	}
	if calls == 0 {
		t.Fatal("nothing crossed the storage codec; this test proves nothing")
	}

	// The version, read back rather than remembered.
	version, checksum, err := client.CurrentVersion(context.Background(), SchemaOwner)
	if err != nil {
		t.Fatalf("current version: %v", err)
	}
	if version != 1 {
		t.Fatalf("recorded version = %d, want 1", version)
	}
	if checksum == "" {
		t.Error("no checksum recorded; a version alone cannot tell a caller " +
			"whether the statements it holds are the ones that ran")
	}

	// And the row is there, read through the same wire.
	rows, err := client.Query(context.Background(), "control_plane_state_read", 0,
		`SELECT started_at, module_version FROM control_plane_state WHERE id = 1`)
	if err != nil {
		t.Fatalf("read state: %v", err)
	}
	if len(rows) != 1 || len(rows[0]) != 2 {
		t.Fatalf("rows = %+v", rows)
	}
	if rows[0][1].Text != "live-test" {
		t.Errorf("module_version = %q, want %q", rows[0][1].Text, "live-test")
	}

	// Applying again is a no-op rather than a failure: the module records
	// (owner, version) and answers already-applied, comparing the checksum of
	// the statements that actually arrived.
	if err := RecordStart(context.Background(), "2026-01-02T00:00:00Z", "live-test"); err != nil {
		t.Fatalf("second start: %v", err)
	}
	if again, _, err := client.CurrentVersion(context.Background(), SchemaOwner); err != nil ||
		again != 1 {
		t.Fatalf("version after a second start = %d (%v), want 1", again, err)
	}
}
