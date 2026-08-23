package postgres

import (
	"bytes"
	"context"
	"errors"
	"strings"
	"testing"
)

// The client and the module's handler are two halves of one wire, so these
// drive the client against a recorded frame rather than against a mock that
// agrees with it by construction.

func decodeRequest(t *testing.T, body []byte) (uint32, string, uint64, string, []Value) {
	t.Helper()
	r := &reader{buf: body}
	op, err := r.u32()
	if err != nil {
		t.Fatalf("op: %v", err)
	}
	if op == opCurrentVersion {
		owner, _ := r.str()
		return op, owner, 0, "", nil
	}
	id, err := r.str()
	if err != nil {
		t.Fatalf("statement_id: %v", err)
	}
	handle, _ := r.u64()
	sql, _ := r.str()
	count, _ := r.u32()
	args := make([]Value, 0, count)
	for index := uint32(0); index < count; index++ {
		value, err := r.value()
		if err != nil {
			t.Fatalf("arg %d: %v", index, err)
		}
		args = append(args, value)
	}
	return op, id, handle, sql, args
}

func okReply(payload ...byte) []byte {
	w := &writer{}
	w.u32(statusOK)
	w.str("")
	w.str("")
	w.buf = append(w.buf, payload...)
	return w.buf
}

func failReply(status uint32, sqlstate, message string) []byte {
	w := &writer{}
	w.u32(status)
	w.str(sqlstate)
	w.str(message)
	return w.buf
}

func TestExecSendsWhatTheModuleExpects(t *testing.T) {
	var seen []byte
	client := New(func(ctx context.Context, body []byte) ([]byte, error) {
		seen = body
		w := &writer{}
		w.u64(3)
		return okReply(w.buf...), nil
	})
	affected, err := client.Exec(context.Background(), "runtime_state_set", 0,
		"UPDATE t SET v = $1 WHERE k = $2", Int(7), Text("k"))
	if err != nil || affected != 3 {
		t.Fatalf("affected = %d, err = %v", affected, err)
	}
	op, id, handle, sql, args := decodeRequest(t, seen)
	if op != opExec || id != "runtime_state_set" || handle != 0 ||
		sql != "UPDATE t SET v = $1 WHERE k = $2" || len(args) != 2 {
		t.Fatalf("request = %d %q %d %q %+v", op, id, handle, sql, args)
	}
	if args[0].Type != typeInt || args[0].Int != 7 ||
		args[1].Type != typeText || args[1].Text != "k" {
		t.Fatalf("arguments lost their types: %+v", args)
	}
}

func TestNilBytesIsNullAndEmptyBytesIsNot(t *testing.T) {
	// The distinction the wire exists for, at the point a caller creates a
	// value. Collapsing them makes a nullable binary column unwritable in one
	// direction: there would be no way to say "store an empty blob".
	if Bytes(nil).Type != typeNull {
		t.Error("a nil slice is not NULL")
	}
	empty := Bytes([]byte{})
	if empty.Type != typeBytes || empty.Bytes == nil {
		t.Error("an empty slice became NULL")
	}
}

func TestAStatementIDIsRequired(t *testing.T) {
	// Required rather than optional: it is the only thing that lets the module
	// say what a caller was doing without parsing SQL, and an optional field is
	// one that rots.
	client := New(func(context.Context, []byte) ([]byte, error) {
		t.Fatal("a statement with no id was sent")
		return nil, nil
	})
	if _, err := client.Exec(context.Background(), "", 0, "SELECT 1"); err == nil {
		t.Fatal("an empty statement_id was accepted")
	}
}

func TestOversizedInputIsRefusedLocally(t *testing.T) {
	// The module enforces its own copy of every limit; this one exists to fail
	// faster and with a better message, not to be the control.
	client := New(func(context.Context, []byte) ([]byte, error) {
		t.Fatal("an oversized request was sent")
		return nil, nil
	})
	ctx := context.Background()
	if _, err := client.Exec(ctx, strings.Repeat("x", MaxStatementIDBytes+1), 0,
		"SELECT 1"); err == nil {
		t.Error("an oversized statement_id was sent")
	}
	if _, err := client.Exec(ctx, "op", 0,
		strings.Repeat("x", MaxStatementBytes+1)); err == nil {
		t.Error("an oversized statement was sent")
	}
	if _, err := client.Exec(ctx, "op", 0, "SELECT 1",
		Text(strings.Repeat("x", MaxCellBytes+1))); err == nil {
		t.Error("an oversized cell was sent")
	}
}

func TestAFailureCarriesItsSQLStateToAClassifier(t *testing.T) {
	// A caller retries a serialization failure, treats a duplicate as an
	// expected answer, and escalates an outage. It can do none of that from a
	// status alone.
	for state, check := range map[string]func(error) bool{
		"23505": IsUniqueViolation,
		"23503": IsForeignKeyViolation,
		"23514": IsCheckViolation,
		"23502": IsNotNullViolation,
		"25P01": IsTransactionClosed,
		"25P02": IsTransactionClosed,
	} {
		client := New(func(context.Context, []byte) ([]byte, error) {
			return failReply(statusStatementFailed, state, "refused"), nil
		})
		_, err := client.Exec(context.Background(), "op", 0, "INSERT INTO t VALUES (1)")
		if err == nil {
			t.Fatalf("%s: no error", state)
		}
		if !check(err) {
			t.Errorf("%s was not classified: %v", state, err)
		}
		// A constraint failure must NOT read as a lost transaction: one leaves
		// the transaction live with a single statement wrong, the other means
		// everything in it is already gone.
		if state == "23505" && IsTransactionClosed(err) {
			t.Error("a unique violation read as a closed transaction")
		}
	}
}

func TestQueryReadsTypedCells(t *testing.T) {
	client := New(func(context.Context, []byte) ([]byte, error) {
		w := &writer{}
		w.u32(3) // width
		w.u32(1) // rows
		_ = w.value(Text("hello"))
		_ = w.value(Value{Type: typeNull})
		_ = w.value(Bool(true))
		return okReply(w.buf...), nil
	})
	row, err := client.QueryRow(context.Background(), "op", 0, "SELECT a, b, c FROM t")
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	if len(row) != 3 || row[0].Text != "hello" || row[1].Type != typeNull ||
		row[2].Type != typeBool || !row[2].Bool {
		t.Fatalf("row = %+v", row)
	}
}

func TestQueryRowSaysWhenThereIsNoRow(t *testing.T) {
	client := New(func(context.Context, []byte) ([]byte, error) {
		w := &writer{}
		w.u32(1)
		w.u32(0)
		return okReply(w.buf...), nil
	})
	if _, err := client.QueryRow(context.Background(), "op", 0, "SELECT a FROM t"); !errors.Is(err, ErrNoRows) {
		t.Fatalf("err = %v, want ErrNoRows", err)
	}
}

func TestInTxRollsBackWhenTheBodyFails(t *testing.T) {
	var ops []uint32
	client := New(func(ctx context.Context, body []byte) ([]byte, error) {
		op, _, _, _, _ := decodeRequest(t, body)
		ops = append(ops, op)
		if op == opBegin {
			w := &writer{}
			w.u64(4242)
			return okReply(w.buf...), nil
		}
		return okReply(), nil
	})
	want := errors.New("the body failed")
	err := client.InTx(context.Background(), "op", func(handle uint64) error {
		if handle != 4242 {
			t.Errorf("handle = %d", handle)
		}
		return want
	})
	if !errors.Is(err, want) {
		t.Fatalf("err = %v, want the body's error rather than the rollback's", err)
	}
	if len(ops) != 2 || ops[0] != opBegin || ops[1] != opRollback {
		t.Fatalf("ops = %v, want begin then rollback", ops)
	}
}

func TestMigrateRefusesWhatCannotBeAMigration(t *testing.T) {
	client := New(func(context.Context, []byte) ([]byte, error) {
		t.Fatal("an invalid migration was sent")
		return nil, nil
	})
	ctx := context.Background()
	if err := client.Migrate(ctx, "", 1, "sum", []string{"SELECT 1"}); err == nil {
		t.Error("an empty owner was accepted")
	}
	if err := client.Migrate(ctx, "db1", 0, "sum", []string{"SELECT 1"}); err == nil {
		t.Error("version zero was accepted")
	}
	if err := client.Migrate(ctx, "db1", 1, "sum", nil); err == nil {
		t.Error("a migration with no statements was accepted")
	}
}

func TestMigrateSendsTheOwnerVersionAndStatements(t *testing.T) {
	var seen []byte
	client := New(func(ctx context.Context, body []byte) ([]byte, error) {
		seen = body
		return okReply(), nil
	})
	statements := []string{"CREATE TABLE a (id BIGINT)", "CREATE INDEX ON a (id)"}
	if err := client.Migrate(context.Background(), "control-plane", 2, "abcd",
		statements); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	r := &reader{buf: seen}
	op, _ := r.u32()
	owner, _ := r.str()
	version, _ := r.u64()
	checksum, _ := r.str()
	count, _ := r.u32()
	if op != opMigrate || owner != "control-plane" || version != 2 ||
		checksum != "abcd" || count != 2 {
		t.Fatalf("migrate frame = %d %q %d %q %d", op, owner, version, checksum, count)
	}
	first, _ := r.str()
	if first != statements[0] {
		t.Fatalf("first statement = %q", first)
	}
}

func TestBytesRoundTripThroughTheCodec(t *testing.T) {
	// Bytes that are not valid UTF-8 anywhere: a digest or a nonce routed
	// through a string is corrupted silently and stops comparing equal to
	// itself.
	raw := []byte{0x00, 0xff, 0xfe, 0x80, 0x7f}
	w := &writer{}
	if err := w.value(Bytes(raw)); err != nil {
		t.Fatalf("encode: %v", err)
	}
	r := &reader{buf: w.buf}
	value, err := r.value()
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if value.Type != typeBytes || !bytes.Equal(value.Bytes, raw) {
		t.Fatalf("bytes did not survive: %#v", value)
	}
}
