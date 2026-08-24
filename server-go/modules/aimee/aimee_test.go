package aimee

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	wire "github.com/JBailes/aimee/server-go/db1"
)

// --- fake database -----------------------------------------------------------

type fakeTx struct {
	committed  bool
	rolledBack bool
	commitErr  error
}

func (t *fakeTx) Exec(context.Context, string, ...any) (Tag, error) {
	return RowsAffected(0), nil
}
func (t *fakeTx) Query(context.Context, string, ...any) (Rows, error) { return nil, nil }
func (t *fakeTx) QueryRow(context.Context, string, ...any) Row        { return nil }
func (t *fakeTx) Commit(context.Context) error {
	t.committed = true
	return t.commitErr
}
func (t *fakeTx) Rollback(context.Context) error {
	t.rolledBack = true
	return nil
}

type fakeDB struct {
	tx       *fakeTx
	beginErr error
}

func (d *fakeDB) Exec(context.Context, string, ...any) (Tag, error) {
	return RowsAffected(0), nil
}
func (d *fakeDB) Query(context.Context, string, ...any) (Rows, error) { return nil, nil }
func (d *fakeDB) QueryRow(context.Context, string, ...any) Row        { return nil }
func (d *fakeDB) Begin(context.Context) (Tx, error) {
	if d.beginErr != nil {
		return nil, d.beginErr
	}
	if d.tx == nil {
		d.tx = &fakeTx{}
	}
	return d.tx, nil
}

func invoke(t *testing.T, f Family, db DB, frame []byte) ([]byte, bus.ModuleStatus) {
	t.Helper()
	return f.Handler(db)(bus.ModuleInvocation{StageID: f.Stage}, frame)
}

// --- codec -------------------------------------------------------------------

// The caller-side codec in server-go/db1 and this one are mirrors. Testing them
// against each other is the only check that actually matters: each is correct
// exactly insofar as the other can read it.
func TestCodecMirrorsTheCallerSide(t *testing.T) {
	cases := [][]string{
		nil,
		{""},
		{"a"},
		{"", "", ""},
		{"session-1", "42", "3.5", "text with spaces"},
		{strings.Repeat("x", 70000)},
	}
	for _, fields := range cases {
		frame, err := wire.EncodeFields(7, fields)
		if err != nil {
			t.Fatalf("caller encode %v: %v", fields, err)
		}
		op, got, ok := DecodeRequest(frame)
		if !ok {
			t.Fatalf("server could not decode a frame the caller encoded: %v", fields)
		}
		if op != 7 || len(got) != len(fields) {
			t.Fatalf("op = %d, fields = %d, want 7 and %d", op, len(got), len(fields))
		}
		for i := range fields {
			if got[i] != fields[i] {
				t.Fatalf("field %d = %q, want %q", i, got[i], fields[i])
			}
		}

		reply := EncodeReply(StatusOK, fields)
		status, back, err := wire.DecodeFields(reply)
		if err != nil {
			t.Fatalf("caller could not decode a reply the server encoded: %v", err)
		}
		if status != StatusOK || len(back) != len(fields) {
			t.Fatalf("status = %d, fields = %d", status, len(back))
		}
	}
}

func TestDecodeRequestRefusesMalformedFrames(t *testing.T) {
	good, err := wire.EncodeFields(1, []string{"a", "bb"})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	corrupt := func(mutate func([]byte) []byte) []byte {
		frame := append([]byte(nil), good...)
		return mutate(frame)
	}
	cases := map[string][]byte{
		"empty":           {},
		"header-only-4":   {1, 0, 0, 0},
		"truncated":       good[:len(good)-1],
		"trailing-bytes":  append(append([]byte(nil), good...), 0xff),
		"count-too-large": corrupt(func(f []byte) []byte { f[4] = 0xff; f[5] = 0xff; f[6] = 0xff; f[7] = 0xff; return f }),
		"length-lies":     corrupt(func(f []byte) []byte { f[8] = 0xff; return f }),
	}
	for name, frame := range cases {
		t.Run(name, func(t *testing.T) {
			if _, _, ok := DecodeRequest(frame); ok {
				t.Fatalf("accepted a malformed frame")
			}
		})
	}
}

// --- dispatch ----------------------------------------------------------------

func echoFamily(run OpFunc, args int, tx bool) Family {
	return Family{
		Name: "test", Event: 11777, Stage: 1,
		Ops: map[uint32]Op{1: {Name: "echo", Args: args, Tx: tx, Run: run}},
	}
}

func okRun(_ context.Context, _ Queryer, fields []string) (uint32, []string, error) {
	return StatusOK, fields, nil
}

func TestArityMismatchIsInvalidNotAPanic(t *testing.T) {
	f := echoFamily(okRun, 2, false)
	frame, _ := wire.EncodeFields(1, []string{"only-one"})
	response, status := invoke(t, f, &fakeDB{}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if got, _, _ := wire.DecodeFields(response); got != StatusInvalid {
		t.Fatalf("status = %d, want %d (invalid)", got, StatusInvalid)
	}
}

func TestVariadicOpAcceptsAnyFieldCount(t *testing.T) {
	f := echoFamily(okRun, -1, false)
	for _, n := range []int{0, 1, 5} {
		fields := make([]string, n)
		frame, _ := wire.EncodeFields(1, fields)
		response, status := invoke(t, f, &fakeDB{}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatalf("n=%d status = %v", n, status)
		}
		if got, back, _ := wire.DecodeFields(response); got != StatusOK || len(back) != n {
			t.Fatalf("n=%d: status %d, %d fields", n, got, len(back))
		}
	}
}

func TestUnknownOpAndWrongStageAreRefusedAtTheBus(t *testing.T) {
	f := echoFamily(okRun, -1, false)
	frame, _ := wire.EncodeFields(99, nil)
	if _, status := invoke(t, f, &fakeDB{}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown op: status = %v, want InvalidRequest", status)
	}
	frame, _ = wire.EncodeFields(1, nil)
	_, status := f.Handler(&fakeDB{})(bus.ModuleInvocation{StageID: f.Stage + 1}, frame)
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong stage: status = %v, want InvalidRequest", status)
	}
}

// A NUL in a field is refused because the store's C callers read these as C strings:
// storing a value they cannot round-trip would be silent truncation on the way
// back out.
func TestFieldWithNulIsRefused(t *testing.T) {
	f := echoFamily(okRun, -1, false)
	// The caller-side encoder refuses this, so the frame is built directly --
	// the module may not assume every peer is the shipped client.
	frame := EncodeReply(1, []string{"before\x00after"})
	response, status := invoke(t, f, &fakeDB{}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if got, _, _ := wire.DecodeFields(response); got != StatusInvalid {
		t.Fatalf("status = %d, want %d (invalid)", got, StatusInvalid)
	}
}

func TestOpErrorBecomesFailedAndIsNotLeakedToTheWire(t *testing.T) {
	f := echoFamily(func(context.Context, Queryer, []string) (uint32, []string, error) {
		return 0, nil, errors.New("connection reset by peer: host=secret-db user=admin")
	}, -1, false)
	frame, _ := wire.EncodeFields(1, nil)
	response, status := invoke(t, f, &fakeDB{}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK with an in-band failure", status)
	}
	got, fields, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got != StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", got, StatusFailed)
	}
	if len(fields) != 0 {
		t.Fatalf("a failure carried %d fields; the error text must not reach the wire", len(fields))
	}
}

// --- transactions ------------------------------------------------------------

func TestTransactionCommitsOnlyOnOK(t *testing.T) {
	t.Run("ok commits", func(t *testing.T) {
		db := &fakeDB{}
		f := echoFamily(okRun, -1, true)
		frame, _ := wire.EncodeFields(1, nil)
		invoke(t, f, db, frame)
		if !db.tx.committed || db.tx.rolledBack {
			t.Fatalf("committed=%v rolledBack=%v, want commit", db.tx.committed, db.tx.rolledBack)
		}
	})

	// The one that matters: an op that REFUSES must not leave its partial work
	// behind. "missing" or "invalid" reported after a write would otherwise
	// commit that write.
	for _, status := range []uint32{StatusMissing, StatusInvalid, StatusTooLong} {
		t.Run("refusal rolls back", func(t *testing.T) {
			db := &fakeDB{}
			f := echoFamily(func(context.Context, Queryer, []string) (uint32, []string, error) {
				return status, nil, nil
			}, -1, true)
			frame, _ := wire.EncodeFields(1, nil)
			response, _ := invoke(t, f, db, frame)
			if db.tx.committed {
				t.Fatalf("status %d committed its transaction", status)
			}
			if !db.tx.rolledBack {
				t.Fatalf("status %d did not roll back", status)
			}
			// The refusal still reaches the caller: rolling back is not the
			// same as turning the answer into a failure.
			if got, _, _ := wire.DecodeFields(response); got != status {
				t.Fatalf("caller saw status %d, want %d", got, status)
			}
		})
	}

	t.Run("error rolls back", func(t *testing.T) {
		db := &fakeDB{}
		f := echoFamily(func(context.Context, Queryer, []string) (uint32, []string, error) {
			return 0, nil, errors.New("boom")
		}, -1, true)
		frame, _ := wire.EncodeFields(1, nil)
		invoke(t, f, db, frame)
		if db.tx.committed || !db.tx.rolledBack {
			t.Fatalf("committed=%v rolledBack=%v, want rollback", db.tx.committed, db.tx.rolledBack)
		}
	})

	t.Run("commit failure is a failure", func(t *testing.T) {
		db := &fakeDB{tx: &fakeTx{commitErr: errors.New("commit lost")}}
		f := echoFamily(okRun, -1, true)
		frame, _ := wire.EncodeFields(1, nil)
		response, _ := invoke(t, f, db, frame)
		if got, _, _ := wire.DecodeFields(response); got != StatusFailed {
			t.Fatalf("status = %d, want %d -- a lost commit is not a success", got, StatusFailed)
		}
	})

	t.Run("begin failure is a failure", func(t *testing.T) {
		db := &fakeDB{beginErr: errors.New("no connection")}
		f := echoFamily(okRun, -1, true)
		frame, _ := wire.EncodeFields(1, nil)
		response, _ := invoke(t, f, db, frame)
		if got, _, _ := wire.DecodeFields(response); got != StatusFailed {
			t.Fatalf("status = %d, want %d", got, StatusFailed)
		}
	})

	t.Run("non-transactional op never begins one", func(t *testing.T) {
		db := &fakeDB{}
		f := echoFamily(okRun, -1, false)
		frame, _ := wire.EncodeFields(1, nil)
		invoke(t, f, db, frame)
		if db.tx != nil {
			t.Fatalf("an op declaring no transaction opened one anyway")
		}
	})
}

// --- scalars -----------------------------------------------------------------

func TestScalarsRoundTripThroughTheWireSpelling(t *testing.T) {
	if v, ok := Atoi(Itoa(-42)); !ok || v != -42 {
		t.Fatalf("int round trip = %d, %v", v, ok)
	}
	if v, ok := Atoi64(I64toa(1 << 40)); !ok || v != 1<<40 {
		t.Fatalf("int64 round trip = %d, %v", v, ok)
	}
	// 'g'/-1 is required here: a cost compared against a cap must survive the
	// trip without drifting in the last bits.
	const cost = 0.1 + 0.2
	if v, ok := Atof(Ftoa(cost)); !ok || v != cost {
		t.Fatalf("float round trip = %v (%v), want exactly %v", v, ok, cost)
	}
	if v, ok := Atob(Btoa(true)); !ok || !v {
		t.Fatalf("bool round trip = %v, %v", v, ok)
	}
	// An unset field arrives as "" and is zero, not a parse error.
	for _, empty := range []string{""} {
		if v, ok := Atoi(empty); !ok || v != 0 {
			t.Fatalf("empty int cell = %d, %v", v, ok)
		}
		if v, ok := Atof(empty); !ok || v != 0 {
			t.Fatalf("empty float cell = %v, %v", v, ok)
		}
	}
	// But garbage is a parse failure, not a silent zero.
	for _, bad := range []string{"12x", "1.2.3", "  7", "0x10"} {
		if _, ok := Atoi(bad); ok {
			t.Fatalf("Atoi accepted %q", bad)
		}
	}
	for _, bad := range []string{"true", "yes", "2", "-1"} {
		if _, ok := Atob(bad); ok {
			t.Fatalf("Atob accepted %q", bad)
		}
	}
}
