package families

import (
	"context"
	"strings"
	"testing"
)

func TestLegacyPrimaryTextChecksCompleteOrderedUTF8Source(t *testing.T) {
	for _, test := range []struct {
		name             string
		size, characters int64
		rows             [][]any
		want             string
		ok               bool
	}{
		{name: "empty", ok: true},
		{name: "exact", size: 5, characters: 3, rows: [][]any{{int64(0), "火\t\n"}}, want: "火\t\n", ok: true},
		{name: "missing", size: 1, characters: 1},
		{name: "wrongindex", size: 1, characters: 1, rows: [][]any{{int64(1), "x"}}},
		{name: "wrongbytes", size: 2, characters: 1, rows: [][]any{{int64(0), "x"}}},
		{name: "wrongcharacters", size: 3, characters: 2, rows: [][]any{{int64(0), "火"}}},
		{name: "invalidUTF8", size: 1, characters: 1, rows: [][]any{{int64(0), string([]byte{255})}}},
		{name: "overbudget", size: legacyPrimaryTextMaxBytes + 1, characters: 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			d := newSessDB()
			d.row = []any{test.size, test.characters}
			d.rows = test.rows
			result, err := legacyPrimaryText(context.Background(), d, "session", "agent", "provider")
			if (err == nil) != test.ok || result != test.want {
				t.Fatal(result, err)
			}
			if !strings.Contains(d.executed[0], "FOR SHARE") {
				t.Fatal("legacy source not locked")
			}
		})
	}
	if !Sessions.Ops[15].Tx {
		t.Fatal("source reads require owner transaction")
	}
}

func TestDeferredPrimaryConstraintIsOneExactHistoricalPlan(t *testing.T) {
	history, err := Migrations()
	if err != nil {
		t.Fatal(err)
	}
	original := history[19]
	alternate, err := deferredPrimaryCellLimit(original)
	if err != nil {
		t.Fatal(err)
	}
	if alternate.Checksum == original.Checksum || checksum(alternate.Statements) != alternate.Checksum {
		t.Fatal("alternate plan checksum not actual SQL")
	}
	restored := strings.Replace(alternate.Statements[0], "CHECK (octet_length(messages_json) <= 1048576) NOT VALID;", "CHECK (octet_length(messages_json) <= 1048576);", 1)
	if restored != original.Statements[0] {
		t.Fatal("historical compatibility changes unrelated SQL")
	}
	if _, err = deferredPrimaryCellLimit(history[18]); err == nil {
		t.Fatal("general historical variant admitted")
	}
}
