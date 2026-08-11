package economizer

import (
	"strings"
	"testing"
)

// Ported one-for-one from src/tests/test_fold_recall.c.

func detect(ix *RecallIndex, turnText string, turn, ttl int) (int, string) {
	var b strings.Builder
	n := ix.Detect(turnText, turn, ttl, &b)
	return n, b.String()
}

func TestRecallAddDedup(t *testing.T) {
	ix := NewRecallIndex()
	ix.Add("/src/foo.c")
	ix.Add("/src/foo.c") // dup
	ix.Add("memory:abc")
	ix.Add("") // ignored
	if ix.Len() != 2 {
		t.Errorf("Len() = %d, want 2", ix.Len())
	}
}

func TestRecallDetectAndTTL(t *testing.T) {
	ix := NewRecallIndex()
	ix.Add("/src/foo.c")
	ix.Add("handle:xyz")

	// turn 1: re-touch foo.c -> surfaced
	n1, o1 := detect(ix, "let me look at /src/foo.c again", 1, 4)
	if n1 != 1 || !strings.Contains(o1, "/src/foo.c") || !strings.Contains(o1, "recall:") {
		t.Fatalf("turn 1: n=%d out=%q", n1, o1)
	}

	// turn 2: re-touch within TTL(4) -> NOT surfaced (anti-thrash)
	if n2, _ := detect(ix, "still in /src/foo.c", 2, 4); n2 != 0 {
		t.Errorf("turn 2: n=%d, want 0 (within TTL)", n2)
	}

	// turn 6: past TTL -> surfaced again
	if n3, _ := detect(ix, "back to /src/foo.c", 6, 4); n3 != 1 {
		t.Errorf("turn 6: n=%d, want 1 (past TTL)", n3)
	}

	// a turn touching neither key surfaces nothing
	if n4, _ := detect(ix, "unrelated chatter", 7, 4); n4 != 0 {
		t.Errorf("turn 7: n=%d, want 0", n4)
	}

	// handle:xyz first touch surfaces
	n5, o5 := detect(ix, "see handle:xyz for details", 8, 4)
	if n5 != 1 || !strings.Contains(o5, "handle:xyz") {
		t.Errorf("turn 8: n=%d out=%q", n5, o5)
	}
}

func TestRecallEmptyAndNil(t *testing.T) {
	ix := NewRecallIndex()
	ix.Add("/x")
	if got := ix.Detect("", 1, 4, nil); got != 0 {
		t.Errorf("empty turn text: %d, want 0", got)
	}
	// detect with a nil sink still updates residency and counts
	if got := ix.Detect("touch /x here", 1, 4, nil); got != 1 {
		t.Errorf("nil sink: %d, want 1", got)
	}
	if got := ix.Detect("touch /x here", 2, 4, nil); got != 0 {
		t.Errorf("nil sink within TTL: %d, want 0", got)
	}
}

// Substring collisions must NOT trigger recall — but exact tokens do match.
func TestRecallWholeTokenMatch(t *testing.T) {
	ix := NewRecallIndex()
	ix.Add("/x")
	ix.Add("memory:42")
	if got := ix.Detect("editing /xyz now", 1, 4, nil); got != 0 {
		t.Errorf("/x must not match /xyz: %d", got)
	}
	if got := ix.Detect("see memory:420 later", 1, 4, nil); got != 0 {
		t.Errorf("memory:42 must not match memory:420: %d", got)
	}
	if got := ix.Detect("open /x please", 1, 4, nil); got != 1 {
		t.Errorf("space-bounded /x should match: %d", got)
	}
	if got := ix.Detect("recall memory:42 now", 2, 4, nil); got != 1 {
		t.Errorf("space-bounded memory:42 should match: %d", got)
	}
}

// Only ADDRESSES become keys: a sha or an issue ref is a fact the closet
// conserves verbatim, but there is nothing to page back in, so a recall hint for
// one would be noise the agent cannot act on.
func TestRecallHarvestAddressesOnly(t *testing.T) {
	ix := NewRecallIndex()
	evicted := "Edited src/modules/git/retry.c and /var/lib/aimee/state, see " +
		"memory:8817 and handle:abc12. Commit " +
		"4a7f19c2b8e30d15f6a2c9b40e7d3814aa9c5162 closes #778, " +
		"retries=5."
	added := ix.AddFromText(evicted)
	if added != ix.Len() || ix.Len() == 0 {
		t.Fatalf("added=%d len=%d", added, ix.Len())
	}

	// Re-touch every candidate at once; whatever is in the table surfaces.
	turn := "look again at src/modules/git/retry.c /var/lib/aimee/state memory:8817 " +
		"handle:abc12 4a7f19c2b8e30d15f6a2c9b40e7d3814aa9c5162 #778 retries=5"
	_, h := detect(ix, turn, 1, 4)

	// Addresses: pageable via code_span_get / memory_get.
	for _, want := range []string{
		"src/modules/git/retry.c", "/var/lib/aimee/state", "memory:8817", "handle:abc12",
	} {
		if !strings.Contains(h, want) {
			t.Errorf("address %q was not surfaced", want)
		}
	}
	// Not addresses: conserved elsewhere, never a recall hint.
	for _, bad := range []string{
		"4a7f19c2b8e30d15f6a2c9b40e7d3814aa9c5162", "#778", "retries=5",
	} {
		if strings.Contains(h, bad) {
			t.Errorf("non-address %q leaked into a recall hint", bad)
		}
	}
}

// Harvesting is idempotent: folding turn after turn must not grow the table with
// duplicates of coordinates already evicted.
func TestRecallHarvestDedupsAcrossCalls(t *testing.T) {
	ix := NewRecallIndex()
	text := "src/a/b.c and memory:7"
	first := ix.AddFromText(text)
	before := ix.Len()
	second := ix.AddFromText(text)
	if first <= 0 || second != 0 || ix.Len() != before {
		t.Errorf("first=%d second=%d len=%d before=%d", first, second, ix.Len(), before)
	}
}
