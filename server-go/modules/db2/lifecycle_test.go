package db2

import (
	"context"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func lifecycleInvocation() bus.ModuleInvocation {
	return bus.ModuleInvocation{StageID: db2contract.StageHealth}
}

// scriptedRows answers each query from a script, so a test can make one probe
// fail while the others succeed.
type scriptedRows struct {
	answers map[string]any
	asked   []string
}

func (s *scriptedRows) queryRow(_ context.Context, query string, _ ...any) HealthRow {
	s.asked = append(s.asked, query)
	answer, present := s.answers[query]
	if !present {
		return fakeRow{err: errors.New("no answer scripted")}
	}
	switch value := answer.(type) {
	case int64:
		return fakeRow{values: []any{value}}
	case error:
		return fakeRow{err: value}
	}
	return fakeRow{err: errors.New("unsupported scripted answer")}
}

func TestPostgresStatusReportsEveryAvailableFact(t *testing.T) {
	script := &scriptedRows{answers: map[string]any{
		sqlActiveConnections: int64(12),
		sqlMaxConnections:    int64(100),
		sqlIsReplica:         int64(1),
		sqlReplicaLag:        int64(4096),
	}}
	backend := NewPGLifecycleBackend(LifecycleSeams{QueryRow: script.queryRow})

	status, err := backend.PostgresStatus(context.Background())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	for _, bit := range []uint32{
		db2contract.PostgresAvailableActive, db2contract.PostgresAvailableMax,
		db2contract.PostgresAvailableRole, db2contract.PostgresAvailableLag,
	} {
		if status.Available&bit == 0 {
			t.Errorf("availability bit %d not set", bit)
		}
	}
	if status.ActiveConnections != 12 || status.MaxConnections != 100 {
		t.Errorf("connections = %d/%d", status.ActiveConnections, status.MaxConnections)
	}
	if status.IsReplica != 1 || status.ReplicaLagBytes != 4096 {
		t.Errorf("replica = %d, lag = %d", status.IsReplica, status.ReplicaLagBytes)
	}
}

// pg_stat_activity is restricted for an unprivileged role. Losing the
// connection count that way must not also cost the caller the replica role it
// could have had.
func TestPostgresStatusOmitsOnlyTheProbeThatFailed(t *testing.T) {
	script := &scriptedRows{answers: map[string]any{
		sqlActiveConnections: errors.New("permission denied for pg_stat_activity"),
		sqlMaxConnections:    int64(100),
		sqlIsReplica:         int64(0),
	}}
	backend := NewPGLifecycleBackend(LifecycleSeams{QueryRow: script.queryRow})

	status, err := backend.PostgresStatus(context.Background())
	if err != nil {
		t.Fatalf("a refused view must not fail the operation: %v", err)
	}
	if status.Available&db2contract.PostgresAvailableActive != 0 {
		t.Error("the refused probe must leave its bit clear")
	}
	if status.ActiveConnections != 0 {
		t.Errorf("a withheld count must stay zero, got %d", status.ActiveConnections)
	}
	if status.Available&db2contract.PostgresAvailableMax == 0 ||
		status.Available&db2contract.PostgresAvailableRole == 0 {
		t.Error("the probes that answered must still be reported")
	}
}

// A primary has no lag to report, and reporting zero would read as "caught up"
// rather than "not applicable".
func TestPostgresStatusDoesNotProbeLagOnAPrimary(t *testing.T) {
	script := &scriptedRows{answers: map[string]any{
		sqlActiveConnections: int64(1),
		sqlMaxConnections:    int64(10),
		sqlIsReplica:         int64(0),
		sqlReplicaLag:        int64(999),
	}}
	backend := NewPGLifecycleBackend(LifecycleSeams{QueryRow: script.queryRow})

	status, err := backend.PostgresStatus(context.Background())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	for _, asked := range script.asked {
		if asked == sqlReplicaLag {
			t.Fatal("lag must not be probed on a primary")
		}
	}
	if status.Available&db2contract.PostgresAvailableLag != 0 {
		t.Error("the lag bit must stay clear on a primary")
	}
	if status.ReplicaLagBytes != 0 {
		t.Errorf("lag = %d, want 0", status.ReplicaLagBytes)
	}
}

// An unrecognised role value is no role at all: reporting it would make a
// caller believe a replica claim the server never made.
func TestPostgresStatusRejectsUnrecognisedRole(t *testing.T) {
	script := &scriptedRows{answers: map[string]any{
		sqlActiveConnections: int64(1),
		sqlMaxConnections:    int64(10),
		sqlIsReplica:         int64(7),
	}}
	backend := NewPGLifecycleBackend(LifecycleSeams{QueryRow: script.queryRow})

	status, err := backend.PostgresStatus(context.Background())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if status.Available&db2contract.PostgresAvailableRole != 0 {
		t.Error("an unrecognised role must leave the role bit clear")
	}
}

func TestLifecycleHandlerServesPostgresStatus(t *testing.T) {
	script := &scriptedRows{answers: map[string]any{
		sqlActiveConnections: int64(3),
		sqlMaxConnections:    int64(50),
		sqlIsReplica:         int64(0),
	}}
	handler := NewLifecycleHandler(NewPGLifecycleBackend(LifecycleSeams{QueryRow: script.queryRow}))

	reply, status := handler(lifecycleInvocation(), db2contract.EncodePostgresStatusRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, decoded, err := db2contract.DecodePostgresStatusReply(reply)
	if err != nil || result != db2contract.ResultOK {
		t.Fatalf("result = %d, err = %v", result, err)
	}
	if decoded.ActiveConnections != 3 || decoded.MaxConnections != 50 {
		t.Errorf("decoded = %+v", decoded)
	}
}

func TestLifecycleHandlerServesPoolStatus(t *testing.T) {
	want := db2contract.PoolStatus{
		Size: 8, InUse: 3, Waiters: 1,
		LeaseGrants: 100, LeaseTimeouts: 2, Stuck: 0, Poisoned: 1,
	}
	handler := NewLifecycleHandler(NewPGLifecycleBackend(LifecycleSeams{
		PoolStats: func(context.Context) (db2contract.PoolStatus, error) { return want, nil },
	}))

	reply, status := handler(lifecycleInvocation(), db2contract.EncodePoolStatusRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, decoded, err := db2contract.DecodePoolStatusReply(reply)
	if err != nil || result != db2contract.ResultOK {
		t.Fatalf("result = %d, err = %v", result, err)
	}
	if decoded != want {
		t.Errorf("decoded = %+v, want %+v", decoded, want)
	}
}

func TestLifecycleHandlerServesHealth(t *testing.T) {
	script := &scriptedRows{}
	backend := NewPGLifecycleBackend(LifecycleSeams{
		QueryRow: func(context.Context, string, ...any) HealthRow {
			return healthRow{}
		},
	})
	_ = script
	reply, status := NewLifecycleHandler(backend)(lifecycleInvocation(),
		db2contract.EncodeHealthRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(reply) == 0 {
		t.Fatal("expected a health reply")
	}
}

// healthRow answers the three booleans the health probe scans.
type healthRow struct{}

func (healthRow) Scan(dest ...any) error {
	if len(dest) != 3 {
		return errors.New("scan arity mismatch")
	}
	for _, target := range dest {
		flag, ok := target.(*bool)
		if !ok {
			return errors.New("unsupported scan target")
		}
		*flag = true
	}
	return nil
}

func TestLifecycleHandlerRejectsWrongStage(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: db2contract.StageLevel3Count}
	_, status := NewLifecycleHandler(NewPGLifecycleBackend(LifecycleSeams{}))(invocation,
		db2contract.EncodeHealthRequest())
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v", status)
	}
}

func TestLifecycleHandlerReportsAbsentBackend(t *testing.T) {
	_, status := NewLifecycleHandler(nil)(lifecycleInvocation(), db2contract.EncodeHealthRequest())
	if status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("status = %v, want capability absent", status)
	}
}
