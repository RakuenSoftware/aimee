package families

import (
	"context"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strings"
	"testing"
)

type cronIRQuery struct {
	coordIRQuery
	exec func(string, []any) (store.Tag, error)
}

func (q cronIRQuery) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	return q.exec(sql, args)
}
func cronIRFixture(t *testing.T) string {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"schema": "aimee.cron-output.v1", "kind": "response", "value": canonical.TextResponse("complete answer")})
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}
func TestCronSnapshotWritesPreserveIndependentLegacyDiscriminator(t *testing.T) {
	snapshot := cronIRFixture(t)
	for _, tc := range []struct {
		name, snapshot string
		valid          bool
	}{
		{"legacy", "", true}, {"typed", snapshot, true}, {"bad snapshot", "AIMEE_CRON_RESPONSE_IR_V1\n{}", false},
		{"unknown kind", `{"schema":"aimee.cron-output.v1","kind":"native","value":{}}`, false},
		{"oversized", strings.Repeat("x", 1048577), false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			called := false
			display := "AIMEE_CRON_RESPONSE_IR_V1\n" + snapshot
			if tc.name == "typed" {
				display = "complete answer"
			}
			q := cronIRQuery{coordIRQuery: coordIRQuery{row: func(sql string, args []any) store.Row {
				called = true
				if sql != cronJobRecordRunSQL || len(args) != 8 || args[4] != display {
					t.Fatalf("wrong display/storage write: %v", args)
				}
				if tc.snapshot == "" && args[7] != nil || tc.snapshot != "" && args[7] != tc.snapshot {
					t.Fatalf("wrong explicit discriminator: %v", args[7])
				}
				return coordIRRow(func(dest ...any) error { *dest[0].(*int64) = 17; return nil })
			}}, exec: func(sql string, args []any) (store.Tag, error) {
				if sql != cronJobTouchLastRunSQL || args[2] != "display-hash" {
					t.Fatal("hash no longer tracks display")
				}
				return store.RowsAffected(1), nil
			}}
			status, _, err := cronJobRecordRun(context.Background(), q, []string{"job", "completed", "0", "0", display, "", "display-hash", tc.snapshot})
			if err != nil || called != tc.valid || (status == store.StatusOK) != tc.valid {
				t.Fatalf("write status=%d called=%v err=%v", status, called, err)
			}
		})
	}
}
func TestCronLatestSnapshotNeverInfersCodecFromOutput(t *testing.T) {
	snapshot := cronIRFixture(t)
	for _, tc := range []struct {
		name, display, snapshot string
		valid                   bool
	}{
		{"legacy marker", "AIMEE_CRON_RESPONSE_IR_V1\n" + snapshot, "", true},
		{"legacy JSON", snapshot, "", true}, {"typed", "complete answer", snapshot, true},
		{"mismatched typed", "wrong display", snapshot, false},
		{"invalid typed cannot fall back", "valid display", "malformed", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			q := coordIRQuery{row: func(sql string, args []any) store.Row {
				if sql != cronJobLatestOutputSQL || len(args) != 1 {
					t.Fatal("wrong snapshot query")
				}
				return coordIRRow(func(dest ...any) error {
					if len(dest) != 2 {
						t.Fatal("missing independent snapshot column")
					}
					*dest[0].(*string) = tc.display
					*dest[1].(*string) = tc.snapshot
					return nil
				})
			}}
			status, fields, err := cronJobLatestOutput(context.Background(), q, []string{"job"})
			if err != nil || (status == store.StatusOK) != tc.valid {
				t.Fatalf("read status=%d err=%v", status, err)
			}
			if tc.valid && (len(fields) != 2 || fields[0] != tc.display || fields[1] != tc.snapshot) {
				t.Fatal("display/snapshot changed")
			}
		})
	}
}

func TestCronSnapshotPreservesWhitespaceAuthoredAnswer(t *testing.T) {
	raw, err := json.Marshal(map[string]any{"schema": "aimee.cron-output.v1", "kind": "response", "value": canonical.TextResponse(" \n\t")})
	if err != nil {
		t.Fatal(err)
	}
	if text, ok := cronOutputIRText(string(raw)); !ok || text != " \n\t" {
		t.Fatalf("authored whitespace changed/refused: %q %v", text, ok)
	}
}
