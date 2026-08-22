package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestArmStatsCastsItsProductSoTheStatementCanRun(t *testing.T) {
	// The cast is for the C, which sends no parameter types and so cannot have
	// the type of a product of two unknowns inferred -- the statement was
	// rejected every time it ran, and no arm ever accumulated anything. pgx
	// sends types, so this module would work without the cast; the assertion is
	// here to keep the two statements textually comparable for the parity gate
	// and to stop an untyped product being copied back into the C.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmStatsUpdateRequest(
		"replay-point", "replay-arm", 0.5, 2.0, 3.0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditArmStatsUpdate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeBanditArmStatsUpdateReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if strings.Contains(store.lastSQL, "$3 * $3") ||
		strings.Contains(store.lastSQL, "$3*$3") {
		t.Errorf("the product is untyped again; harmless here but the form the C "+
			"cannot run: %q", store.lastSQL)
	}
	if strings.Count(store.lastSQL, "$3::double precision") < 4 {
		t.Errorf("not every use of the reward is typed: %q", store.lastSQL)
	}
}

func TestArmStatsReportsAFailedWrite(t *testing.T) {
	// The C discards the step result and returns success regardless, which is
	// how a statement that never ran stayed invisible for as long as it did.
	store := &fakeStore{execErr: errors.New("operator is not unique")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmStatsUpdateRequest(
		"replay-point", "replay-arm", 0.5, 2.0, 3.0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditArmStatsUpdate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeBanditArmStatsUpdateReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d for a statement that failed, want 0", acknowledged)
	}
}

func TestArmStatsAccumulatesEvidenceAndReplacesBelief(t *testing.T) {
	// The sums are evidence and only grow; the posterior is the caller's
	// current belief given all of it, recomputed and handed over whole.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmStatsUpdateRequest(
		"replay-point", "replay-arm", 0.5, 2.0, 3.0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageBanditArmStatsUpdate), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	for _, accumulated := range []string{
		"n_decisions = bandit_arm_stats.n_decisions + 1",
		"n_rewards = bandit_arm_stats.n_rewards + 1",
		"sum_reward = bandit_arm_stats.sum_reward + $3",
	} {
		if !strings.Contains(store.lastSQL, accumulated) {
			t.Errorf("%q is no longer accumulated: %q", accumulated, store.lastSQL)
		}
	}
	if !strings.Contains(store.lastSQL, "posterior_alpha = $4") ||
		strings.Contains(store.lastSQL, "posterior_alpha = bandit_arm_stats.posterior_alpha") {
		t.Errorf("the posterior is being accumulated rather than replaced: %q",
			store.lastSQL)
	}
}

func TestPromotionSetReplacesRatherThanAccumulates(t *testing.T) {
	// A decision point pinned to two arms is not pinned.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditPromotionSetRequest(
		"replay-point", "new-arm", "old-arm")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageBanditPromotionSet), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (decision_point) DO UPDATE") {
		t.Errorf("a second promotion would insert rather than replace: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 3 || store.lastArgs[2] != "old-arm" {
		t.Fatalf("args = %v -- the rollback arm is not stored", store.lastArgs)
	}
}

func TestExploreStatsTreatAZeroWindowAsAnyTime(t *testing.T) {
	// The empty cutoff is what makes that work: every timestamp compares
	// greater than the empty string, so zero means all of history rather than
	// none of it.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(10), idPtr(3)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditExploreStatsRequest("replay-point", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditExploreStats), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	explore, total, decodeErr := db2contract.DecodeBanditExploreStatsReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	// COUNT is the first column and SUM the second, while the reply names
	// explore first: swapping them would report an exploration rate above one.
	if total != 10 || explore != 3 {
		t.Fatalf("explore = %d, total = %d -- the columns are crossed",
			explore, total)
	}
	if !strings.Contains(store.lastSQL, "ELSE '' END") {
		t.Errorf("a zero window no longer means any time: %q", store.lastSQL)
	}
}

func TestExploreStatsSurviveAnEmptyWindow(t *testing.T) {
	// SUM over no rows is NULL while COUNT is zero, so the two columns behave
	// differently even though both are counts.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(0), (*int64)(nil)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditExploreStatsRequest("quiet-point", 3600)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditExploreStats), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	explore, total, decodeErr := db2contract.DecodeBanditExploreStatsReply(body)
	if decodeErr != nil || explore != 0 || total != 0 {
		t.Fatalf("explore = %d, total = %d", explore, total)
	}
}
