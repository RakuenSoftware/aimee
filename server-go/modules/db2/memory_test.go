package db2

import (
	"context"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

// fakeMemory records what it was asked and answers what the test dictates.
type fakeMemory struct {
	level3, level2, orphaned, session uint32
	total                             uint64
	exists, tierPairExists            bool
	found                             bool
	id                                uint64
	err                               error

	gotSession, gotKey, gotKind string
	gotTierA, gotTierB          string

	effStats           db2contract.EffectivenessStats
	l2IDs              []uint64
	retentionDeleted   map[string]uint32
	demoted            uint32
	gotThreshold       float64
	gotLowThreshold    float64
	gotEffectivenessID uint64
	gotEffectiveness   float64
	cleared            bool
}

func (f *fakeMemory) Level3Count(context.Context) (uint32, error) {
	return f.level3, f.err
}
func (f *fakeMemory) Level2Count(context.Context) (uint32, error) {
	return f.level2, f.err
}
func (f *fakeMemory) OrphanedL0Count(context.Context) (uint32, error) {
	return f.orphaned, f.err
}
func (f *fakeMemory) TotalCount(context.Context) (uint64, error) {
	return f.total, f.err
}
func (f *fakeMemory) SessionL2Count(_ context.Context, s string) (uint32, error) {
	f.gotSession = s
	return f.session, f.err
}
func (f *fakeMemory) KeyExists(_ context.Context, key string) (bool, error) {
	f.gotKey = key
	return f.exists, f.err
}
func (f *fakeMemory) FindIDByKeyKind(_ context.Context, key, kind string) (bool, uint64, error) {
	f.gotKey, f.gotKind = key, kind
	return f.found, f.id, f.err
}
func (f *fakeMemory) KeyExistsInTierPair(_ context.Context, key, a, b string) (bool, error) {
	f.gotKey, f.gotTierA, f.gotTierB = key, a, b
	return f.tierPairExists, f.err
}
func (f *fakeMemory) SetEffectiveness(_ context.Context, id uint64, v float64) error {
	f.gotEffectivenessID, f.gotEffectiveness = id, v
	return f.err
}
func (f *fakeMemory) ClearEffectiveness(_ context.Context, id uint64) error {
	f.gotEffectivenessID, f.cleared = id, true
	return f.err
}
func (f *fakeMemory) RetentionDelete(_ context.Context, sensitivity string, days uint32) (uint32, error) {
	if f.retentionDeleted == nil {
		f.retentionDeleted = map[string]uint32{}
	}
	f.retentionDeleted[sensitivity] = days
	switch sensitivity {
	case db2contract.RetentionRestricted:
		return 2, f.err
	case db2contract.RetentionSensitive:
		return 3, f.err
	}
	return 0, f.err
}
func (f *fakeMemory) DemoteLowEffectiveness(_ context.Context, threshold float64) (uint32, error) {
	f.gotThreshold = threshold
	return f.demoted, f.err
}
func (f *fakeMemory) EffectivenessStats(_ context.Context, low float64) (db2contract.EffectivenessStats, error) {
	f.gotLowThreshold = low
	return f.effStats, f.err
}
func (f *fakeMemory) ListL2MemoryIDs(_ context.Context, max uint32) ([]uint64, error) {
	if uint32(len(f.l2IDs)) > max {
		return f.l2IDs[:max], f.err
	}
	return f.l2IDs, f.err
}

func memoryInvocation() bus.ModuleInvocation {
	return bus.ModuleInvocation{StageID: db2contract.StageLevel3Count}
}

func TestMemoryHandlerServesEveryCountOperation(t *testing.T) {
	backend := &fakeMemory{level3: 3, level2: 2, orphaned: 7, total: 99, session: 5}
	handler := NewMemoryHandler(backend)

	t.Run("level3", func(t *testing.T) {
		reply, status := handler(memoryInvocation(), db2contract.EncodeLevel3CountRequest())
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		got, err := db2contract.DecodeLevel3CountReply(reply)
		if err != nil || got != 3 {
			t.Fatalf("count = %d, err = %v", got, err)
		}
	})

	t.Run("level2", func(t *testing.T) {
		reply, status := handler(memoryInvocation(), db2contract.EncodeLevel2CountRequest())
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		got, err := db2contract.DecodeLevel2CountReply(reply)
		if err != nil || got != 2 {
			t.Fatalf("count = %d, err = %v", got, err)
		}
	})

	t.Run("orphaned l0", func(t *testing.T) {
		reply, status := handler(memoryInvocation(), db2contract.EncodeOrphanedL0CountRequest())
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		got, err := db2contract.DecodeOrphanedL0CountReply(reply)
		if err != nil || got != 7 {
			t.Fatalf("count = %d, err = %v", got, err)
		}
	})

	t.Run("total", func(t *testing.T) {
		reply, status := handler(memoryInvocation(), db2contract.EncodeTotalCountRequest())
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		got, err := db2contract.DecodeTotalCountReply(reply)
		if err != nil || got != 99 {
			t.Fatalf("count = %d, err = %v", got, err)
		}
	})
}

// The session argument has to reach the backend intact: an operation that
// counted the wrong session would answer plausibly and wrongly.
func TestMemoryHandlerPassesSessionThrough(t *testing.T) {
	backend := &fakeMemory{session: 4}
	request, err := db2contract.EncodeSessionL2CountRequest("sess-abc")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.gotSession != "sess-abc" {
		t.Errorf("backend saw session %q", backend.gotSession)
	}
	got, err := db2contract.DecodeSessionL2CountReply(reply)
	if err != nil || got != 4 {
		t.Fatalf("count = %d, err = %v", got, err)
	}
}

func TestMemoryHandlerKeyExists(t *testing.T) {
	for _, exists := range []bool{true, false} {
		backend := &fakeMemory{exists: exists}
		request, err := db2contract.EncodeKeyExistsRequest("k")
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		got, err := db2contract.DecodeKeyExistsReply(reply)
		if err != nil {
			t.Fatalf("decode: %v", err)
		}
		if want := boolReply(exists); got != want {
			t.Errorf("exists=%v encoded %d, want %d", exists, got, want)
		}
	}
}

func TestMemoryHandlerFindIDByKeyKind(t *testing.T) {
	backend := &fakeMemory{found: true, id: 4242}
	request, err := db2contract.EncodeFindIDByKeyKindRequest("k", "fact")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.gotKey != "k" || backend.gotKind != "fact" {
		t.Errorf("backend saw key=%q kind=%q", backend.gotKey, backend.gotKind)
	}
	found, id, err := db2contract.DecodeFindIDByKeyKindReply(reply)
	if err != nil || found != 1 || id != 4242 {
		t.Fatalf("found=%d id=%d err=%v", found, id, err)
	}
}

func TestMemoryHandlerKeyExistsInTierPair(t *testing.T) {
	backend := &fakeMemory{tierPairExists: true}
	request, err := db2contract.EncodeKeyExistsInTierPairRequest("k", "L2", "L3")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	reply, status := NewMemoryHandler(backend)(memoryInvocation(), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.gotTierA != "L2" || backend.gotTierB != "L3" {
		t.Errorf("backend saw tiers %q,%q", backend.gotTierA, backend.gotTierB)
	}
	got, err := db2contract.DecodeKeyExistsInTierPairReply(reply)
	if err != nil || got != 1 {
		t.Fatalf("exists=%d err=%v", got, err)
	}
}

func TestMemoryHandlerRejectsWrongStage(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: db2contract.StageHealth}
	_, status := NewMemoryHandler(&fakeMemory{})(invocation, db2contract.EncodeLevel3CountRequest())
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want invalid request", status)
	}
}

// A missing backend is a capability the module does not currently have, which
// is a different answer from a malformed request.
func TestMemoryHandlerReportsAbsentBackend(t *testing.T) {
	_, status := NewMemoryHandler(nil)(memoryInvocation(), db2contract.EncodeLevel3CountRequest())
	if status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("status = %v, want capability absent", status)
	}
}

func TestMemoryHandlerRejectsMalformedEnvelope(t *testing.T) {
	for _, request := range [][]byte{nil, {}, {1, 2, 3}} {
		_, status := NewMemoryHandler(&fakeMemory{})(memoryInvocation(), request)
		if status != bus.ModuleStatusInvalidRequest {
			t.Errorf("request %v: status = %v, want invalid request", request, status)
		}
	}
}

func TestMemoryHandlerReportsBackendFailure(t *testing.T) {
	backend := &fakeMemory{err: errors.New("database is on fire")}
	_, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeLevel3CountRequest())
	if status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

// A count that clears the database but not the contract's bound must fail
// rather than answer with a truncated number.
func TestMemoryHandlerRefusesOutOfBoundCount(t *testing.T) {
	backend := &fakeMemory{level3: db2contract.Level3CountMax + 1}
	_, status := NewMemoryHandler(backend)(memoryInvocation(), db2contract.EncodeLevel3CountRequest())
	if status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want internal", status)
	}
}

// --- backend tests, over the QueryRowFunc seam ---

type fakeRow struct {
	values []any
	err    error
}

func (r fakeRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if len(dest) != len(r.values) {
		return errors.New("scan arity mismatch")
	}
	for i, v := range r.values {
		switch d := dest[i].(type) {
		case *int64:
			*d = v.(int64)
		case *int:
			*d = v.(int)
		default:
			return errors.New("unsupported scan target")
		}
	}
	return nil
}

func TestPGMemoryBackendCounts(t *testing.T) {
	var gotQuery string
	backend := NewPGMemoryBackend(MemorySeams{QueryRow: func(_ context.Context, query string, _ ...any) HealthRow {
		gotQuery = query
		return fakeRow{values: []any{int64(12)}}
	}})
	got, err := backend.Level3Count(context.Background())
	if err != nil || got != 12 {
		t.Fatalf("count = %d, err = %v", got, err)
	}
	if gotQuery != sqlLevel3Count {
		t.Errorf("query = %q", gotQuery)
	}
}

// A negative COUNT would wrap to an enormous number: a wrong answer that looks
// like a right one.
func TestPGMemoryBackendRefusesNegativeCount(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{QueryRow: func(context.Context, string, ...any) HealthRow {
		return fakeRow{values: []any{int64(-1)}}
	}})
	if _, err := backend.Level3Count(context.Background()); err == nil {
		t.Fatal("expected an error for a negative count")
	}
}

// No rows means absent, not broken.
func TestPGMemoryBackendExistsFoldsNoRows(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{QueryRow: func(context.Context, string, ...any) HealthRow {
		return fakeRow{err: pgx.ErrNoRows}
	}})
	exists, err := backend.KeyExists(context.Background(), "k")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if exists {
		t.Error("expected absent")
	}
}

// The C implementation answers 0 for empty arguments before it reaches the
// database. `source_session = ”` is a legal query that would return a real
// count for a caller that meant to name a session.
func TestPGMemoryBackendRefusesEmptyArgumentsWithoutQuerying(t *testing.T) {
	queried := false
	backend := NewPGMemoryBackend(MemorySeams{QueryRow: func(context.Context, string, ...any) HealthRow {
		queried = true
		return fakeRow{values: []any{int64(1)}}
	}})
	ctx := context.Background()

	if n, err := backend.SessionL2Count(ctx, ""); n != 0 || err != nil {
		t.Errorf("SessionL2Count = %d, %v", n, err)
	}
	if ok, err := backend.KeyExists(ctx, ""); ok || err != nil {
		t.Errorf("KeyExists = %v, %v", ok, err)
	}
	if ok, _, err := backend.FindIDByKeyKind(ctx, "k", ""); ok || err != nil {
		t.Errorf("FindIDByKeyKind = %v, %v", ok, err)
	}
	if ok, err := backend.KeyExistsInTierPair(ctx, "k", "L2", ""); ok || err != nil {
		t.Errorf("KeyExistsInTierPair = %v, %v", ok, err)
	}
	if queried {
		t.Error("an empty argument must not reach the database")
	}
}

func TestPGMemoryBackendWithoutQuerier(t *testing.T) {
	backend := NewPGMemoryBackend(MemorySeams{})
	if _, err := backend.Level3Count(context.Background()); !errors.Is(err, ErrNoQuerier) {
		t.Fatalf("err = %v, want ErrNoQuerier", err)
	}
}
