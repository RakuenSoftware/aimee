package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestTriggerReadsSeeOnlyArmedReminders(t *testing.T) {
	// A reminder that has fired or been cancelled is not something to fire
	// again, and the three trigger reads exist to answer what should fire now.
	for _, query := range []string{
		prospectiveArmedQuery, prospectiveByEntityQuery,
		prospectiveByFileQuery, prospectiveByTriggerQuery,
	} {
		if !strings.Contains(query, "state = 'armed'") {
			t.Errorf("a fired reminder could fire again: %q", query)
		}
	}
	// The maintenance view is the exception: it shows every state, which is
	// what makes it the maintenance view.
	if !strings.Contains(prospectiveListQuery, "($2 = '' OR state = $2)") {
		t.Errorf("the list can no longer show every state: %q", prospectiveListQuery)
	}
}

func TestEveryProspectiveReadOrdersTheSameWay(t *testing.T) {
	// Newest first, with the identifier breaking ties so two reminders created
	// in the same second come back in a stable order.
	for _, query := range []string{
		prospectiveListQuery, prospectiveArmedQuery, prospectiveByEntityQuery,
		prospectiveByFileQuery, prospectiveByTriggerQuery,
	} {
		if !strings.Contains(query, "ORDER BY created_at DESC, id DESC") {
			t.Errorf("the ordering changed: %q", query)
		}
	}
}

func TestTriggerQueryDropsWhatWouldMatchEverything(t *testing.T) {
	// Broad chatter should not fire every armed reminder, so common and short
	// words are dropped -- and each surviving word is a prefix match, so
	// "rotating" in the turn finds a reminder about rotation.
	if got := buildTriggerQuery("Please rotate the DB2 key"); got !=
		"please:* | rotate:* | db2:* | key:*" {
		t.Fatalf("query = %q", got)
	}
	if got := buildTriggerQuery("it is a the of"); got != "" {
		t.Fatalf("stop words survived: %q", got)
	}
	if got := buildTriggerQuery("go to it"); got != "" {
		t.Fatalf("short words survived: %q", got)
	}
	// A word repeated in the turn is one term: the query is an OR, so
	// repeating it changes nothing but the length.
	if got := buildTriggerQuery("rotate rotate rotate"); got != "rotate:*" {
		t.Fatalf("repeats survived: %q", got)
	}
}

func TestATurnWithNothingSearchableAsksNothing(t *testing.T) {
	// An empty tsquery either fails to parse or matches everything, and a
	// reminder firing on "it is the a" would be worse than one not firing.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveByTriggerTermsRequest("it is a the", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageProspectiveByTriggerTerms), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr :=
		db2contract.DecodeProspectiveByTriggerTermsReply(body)
	if decodeErr != nil || len(found) != 0 {
		t.Fatalf("rows = %+v", found)
	}
	if store.lastSQL != "" {
		t.Errorf("a query ran for a turn with nothing in it: %q", store.lastSQL)
	}
}

func TestTriggerSearchRecomputesInEnglish(t *testing.T) {
	// The stored tsvector column is built with the 'simple' configuration, so
	// recomputing with 'english' is what lets a prefix query pick up
	// morphology. It is the difference between a reminder firing and not.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "when the key rotates", "check the vault", "vault",
			"docs/a.md", "once", "armed", "", "session-1", int64(0), "",
			"2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveByTriggerTermsRequest(
		"we should rotate the key", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageProspectiveByTriggerTerms), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr :=
		db2contract.DecodeProspectiveByTriggerTermsReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].ProspectiveID != 4 ||
		found[0].ActionText != "check the vault" {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "to_tsvector('english', trigger_text)") {
		t.Errorf("the search no longer picks up morphology: %q", store.lastSQL)
	}
	if store.lastArgs[1] != "should:* | rotate:* | key:*" {
		t.Errorf("query = %v", store.lastArgs[1])
	}
}

func TestAnchorReadsDifferOnCase(t *testing.T) {
	// An entity anchor is lowered on the stored side because the caller is
	// expected to have lowered its own -- the field is named for it. A path is
	// a path, and two files differing only in case are two files.
	if !strings.Contains(prospectiveByEntityQuery, "LOWER(anchor_entity) = $2") {
		t.Errorf("the entity anchor is no longer case-folded: %q",
			prospectiveByEntityQuery)
	}
	if strings.Contains(prospectiveByFileQuery, "LOWER(anchor_file)") {
		t.Errorf("two distinct paths would collide: %q", prospectiveByFileQuery)
	}
}

func TestProspectiveRowsCarryEveryColumn(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "when the key rotates", "check the vault", "vault",
			"docs/a.md", "once", "armed", "2026-06-01T00:00:00Z", "session-1",
			int64(2), "2026-02-01T00:00:00Z", "2026-01-01T00:00:00Z",
			"2026-01-02T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProspectiveListRequest("armed", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProspectiveList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeProspectiveListReply(body)
	if decodeErr != nil || len(found) != 1 {
		t.Fatalf("rows = %+v", found)
	}
	row := found[0]
	if row.ProspectiveID != 4 || row.TriggerText != "when the key rotates" ||
		row.ActionText != "check the vault" || row.AnchorEntity != "vault" ||
		row.AnchorFile != "docs/a.md" || row.Recurrence != "once" ||
		row.State != "armed" || row.ValidUntil != "2026-06-01T00:00:00Z" ||
		row.SourceSession != "session-1" || row.TriggerCount != 2 ||
		row.LastTriggeredAt != "2026-02-01T00:00:00Z" ||
		row.CreatedAt == "" || row.UpdatedAt == "" {
		t.Fatalf("row = %+v", row)
	}
	if store.lastArgs[1] != "armed" {
		t.Errorf("the state filter was not passed: %v", store.lastArgs)
	}
}
