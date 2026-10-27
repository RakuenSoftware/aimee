package families

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

func TestCoordQueueMessageCustodyAndLegacyStorageImport(t *testing.T) {
	original := ` {"role":"user","blocks":[{"type":"text","text":"look here"},{"type":"image","media_type":"image/png","media_ref":"https://fixture.invalid/a.png"}]} `
	if got, ok := coordTaskMessage(original, false); !ok || got != original {
		t.Fatalf("canonical bytes were changed or refused: %q", got)
	}
	for _, text := range []string{"Legacy task: 雪\nquotes: \"task\"", original, `{"role":"user","blocks":"invalid"}`, `{"role":"assistant","blocks":[{"type":"text","text":"literal text"}]}`, `{"role":`} {
		got, ok := coordTaskMessage(text, true)
		assertCoordLiteral(t, got, text, ok)
	}
	for _, bad := range []string{`plain text`, `{"role":"user","blocks":"invalid"}`, `{"role":"assistant","blocks":[{"type":"text","text":"wrong role"}]}`} {
		if _, ok := coordTaskMessage(bad, false); ok {
			t.Fatalf("invalid new task accepted: %q", bad)
		}
	}
	for _, bad := range []string{strings.Repeat("x", coordMessageMaxBytes+1), ""} {
		for _, legacy := range []bool{false, true} {
			if _, ok := coordTaskMessage(bad, legacy); ok {
				t.Fatal("empty or oversized task accepted")
			}
		}
	}
}

func assertCoordLiteral(t *testing.T, got, want string, ok bool) {
	t.Helper()
	var message canonical.Message
	if !ok || !canonical.ValidMessage(json.RawMessage(got)) || json.Unmarshal([]byte(got), &message) != nil || len(message.Blocks) != 1 || message.Blocks[0].Text == nil || *message.Blocks[0].Text != want {
		t.Fatalf("legacy storage import changed literal text: %q", got)
	}
}

type coordIRRow func(...any) error

func (r coordIRRow) Scan(dest ...any) error { return r(dest...) }

type coordIRQuery struct {
	store.Queryer
	row func(string, []any) store.Row
}

func (q coordIRQuery) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	return q.row(sql, args)
}

func TestCoordDispatchUsesStorageMarker(t *testing.T) {
	canonicalText := ` {"role":"user","blocks":[{"type":"text","text":"task"}]} `
	for _, tc := range []struct {
		name            string
		hasIR           bool
		message, legacy string
		valid           bool
	}{
		{"canonical", true, canonicalText, "old text must not win", true},
		{"legacy JSON", false, "", canonicalText, true},
		{"legacy malformed JSON", false, "", `{"role":`, true},
		{"bad IR cannot fall back", true, "invalid", "valid legacy text", false},
		{"empty IR cannot fall back", true, "", "valid legacy text", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			q := coordIRQuery{row: func(sql string, args []any) store.Row {
				if sql != coordTaskGetDispatchSQL || len(args) != 1 || args[0] != int64(7) {
					t.Fatal("wrong dispatch lookup")
				}
				return coordIRRow(func(dest ...any) error {
					if len(dest) != 7 {
						t.Fatalf("storage marker missing: %d columns", len(dest))
					}
					*dest[0].(*string) = "execute"
					*dest[1].(*bool) = tc.hasIR
					for i, value := range []string{tc.message, tc.legacy, `[]`, "/tmp", "engineer"} {
						*dest[i+2].(*string) = value
					}
					return nil
				})
			}}
			status, fields, err := coordTaskGetDispatch(context.Background(), q, []string{"7"})
			if err != nil {
				t.Fatal(err)
			}
			if !tc.valid {
				if status != store.StatusInvalid {
					t.Fatalf("invalid IR accepted: %d", status)
				}
				return
			}
			if status != store.StatusOK || len(fields) != coordDispatchCells {
				t.Fatalf("dispatch failed: %d %v", status, fields)
			}
			if tc.hasIR {
				if fields[1] != tc.message {
					t.Fatal("canonical message changed")
				}
			} else {
				assertCoordLiteral(t, fields[1], tc.legacy, true)
			}
		})
	}
}

func TestCoordNewWritesUseOnlyCanonicalColumn(t *testing.T) {
	message := `{"role":"user","blocks":[{"type":"text","text":"task"}]}`
	for _, tc := range []struct {
		name, step, message string
		valid, nullable     bool
	}{
		{"canonical", "0", message, true, false},
		{"plan placeholder", "2", "", true, true},
		{"plain text", "0", "task", false, false},
		{"empty task", "0", "", false, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			called := false
			q := coordIRQuery{row: func(sql string, args []any) store.Row {
				called = true
				if sql != coordTaskAddSQL || !strings.Contains(sql, "role, message_ir, cwd") {
					t.Fatal("new write targets legacy prompt")
				}
				if tc.nullable {
					if args[4] != nil {
						t.Fatal("placeholder must retain NULL marker")
					}
				} else if args[4] != message {
					t.Fatal("canonical bytes changed")
				}
				return coordIRRow(func(dest ...any) error { *dest[0].(*int64) = 9; return nil })
			}}
			status, _, err := coordTaskAdd(context.Background(), q, []string{"1", tc.step, "[]", "execute", tc.message, "/tmp", "engineer"})
			if err != nil || called != tc.valid || (status == store.StatusOK) != tc.valid {
				t.Fatalf("write: status=%d called=%v err=%v", status, called, err)
			}
		})
	}
}
