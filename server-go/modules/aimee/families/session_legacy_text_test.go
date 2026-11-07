package families

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"
	"unicode/utf8"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
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

func TestLegacyPrimaryGetLatestMatchesDeclaredCRowOrder(t *testing.T) {
	const transcript = `[{"role":"user","content":"retained literal 火 transcript"}]`
	fields := map[string]string{"session_id": "session", "agent_name": "agent", "provider": "anthropic",
		"created_at": "2026-10-01 00:00:00", "updated_at": "2026-10-02 00:00:00", "messages_json": transcript}
	db := newSessDB()
	db.primaryRow = []any{fields["session_id"], fields["agent_name"], fields["provider"], fields["created_at"], fields["updated_at"]}
	db.row = []any{int64(len(transcript)), int64(utf8.RuneCountInString(transcript))}
	db.rows = [][]any{{int64(0), transcript}}
	status, cells := sessCall(t, db, opPrimarySessionGetLatest, []string{"session"})
	if status != store.StatusOK {
		t.Fatalf("primary latest status %d", status)
	}
	// The descriptor is also the C DTO contract; assert the real Go handler's
	// wire result against it, rather than restating the C fixture's order.
	raw, err := os.ReadFile("../operations.json")
	if err != nil {
		t.Fatal(err)
	}
	var catalog struct {
		Operations []struct {
			Family string `json:"family"`
			ID     uint32 `json:"id"`
			Reply  struct {
				Fields []struct {
					Name string `json:"name"`
				} `json:"fields"`
			} `json:"reply"`
		} `json:"operations"`
	}
	if err := json.Unmarshal(raw, &catalog); err != nil {
		t.Fatal(err)
	}
	for _, op := range catalog.Operations {
		if op.Family != "sessions" || op.ID != opPrimarySessionGetLatest {
			continue
		}
		if len(cells) != len(op.Reply.Fields) || len(cells) != len(fields) {
			t.Fatal("primary row shape", cells)
		}
		for i, field := range op.Reply.Fields {
			if cells[i] != fields[field.Name] {
				t.Fatalf("C field %s received %q, want %q", field.Name, cells[i], fields[field.Name])
			}
		}
		return
	}
	t.Fatal("primary row operation missing from shared descriptor")
}
