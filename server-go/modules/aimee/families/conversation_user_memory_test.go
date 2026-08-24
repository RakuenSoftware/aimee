package families

import (
	"context"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Every recall must gate on the expiry.
//
// The C consulted valid_until nowhere: nothing wrote it and nothing read it, so
// a memory given an expiry was recalled forever regardless of it. The danger in
// fixing it is fixing it in ONE recall -- the two select different rows and are
// easy to edit apart, and a caller would have no way to tell which section had
// been left honouring the column and which had not.
//
// This is a structural check on purpose: what the clause MEANS is settled on a
// real server in scripts/test-family-conversation.sql, which inserts a lapsed
// memory and asserts it is not returned. What can only be checked here is that
// both statements are built from the same clause rather than each carrying its
// own copy to drift.
func TestEveryRecallIsBuiltFromTheSharedLiveClause(t *testing.T) {
	if !strings.Contains(userRecallLiveSQL, "valid_until") {
		t.Fatalf("the shared clause does not mention the expiry at all: %s", userRecallLiveSQL)
	}
	if !strings.Contains(userRecallLiveSQL, "lifecycle_state") {
		t.Fatalf("the shared clause dropped the lifecycle gate: %s", userRecallLiveSQL)
	}
	for name, sql := range map[string]string{
		"identity":    userRecallIdentitySQL,
		"preferences": userRecallPreferencesSQL,
	} {
		if !strings.Contains(sql, userRecallLiveSQL) {
			t.Errorf("the %s recall does not use the shared live clause, so the two "+
				"can drift on which memories count as current:\n%s", name, sql)
		}
	}
}

// A lapsed memory that is written again is asserted again. Reviving it while
// leaving the old expiry in place would set lifecycle_state = 'active' and
// still have every recall skip it -- active and invisible, which is the most
// confusing state the table can hold.
func TestRevivingAMemoryClearsItsLapsedExpiry(t *testing.T) {
	if !strings.Contains(userMemoryUpsertSQL, "lifecycle_state = 'active'") {
		t.Fatalf("the upsert no longer revives a retired memory")
	}
	if !strings.Contains(userMemoryUpsertSQL, "valid_until     = NULL") {
		t.Fatalf("the upsert revives a memory without clearing its expiry, so the "+
			"revived memory is active and still not recalled:\n%s", userMemoryUpsertSQL)
	}
}

// An unknown recall section is refused rather than answered with an empty list.
// The C returned zero rows for it, which a caller cannot tell apart from a
// section that is genuinely empty, and the two mean very different things.
func TestUnknownRecallSectionIsRefused(t *testing.T) {
	for _, section := range []string{"0", "3", "-1", "not a number"} {
		status, _, err := userMemoryListRecall(context.Background(), nil, []string{section, "10"})
		if err != nil {
			t.Fatalf("section %q: unexpected error %v", section, err)
		}
		if status != store.StatusInvalid {
			t.Errorf("section %q answered %d, want INVALID", section, status)
		}
	}
}
