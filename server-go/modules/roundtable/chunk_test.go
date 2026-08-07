package roundtable

import (
	"strings"
	"testing"
)

// Ported from src/tests/test_roundtable_pipeline_chunk.c. The hashing assertions
// stay on the C side, since the hash deliberately did not move.

func TestChunkNeeded(t *testing.T) {
	if ChunkNeeded("short", 100) {
		t.Fatal("under budget must not need chunking")
	}
	if !ChunkNeeded(strings.Repeat("x", 200), 100) {
		t.Fatal("over budget must need chunking")
	}
	// No budget means no chunking, not "chunk everything".
	if ChunkNeeded(strings.Repeat("x", 200), 0) {
		t.Fatal("a zero budget must not request chunking")
	}
	if ChunkNeeded("", 100) {
		t.Fatal("empty origin must not need chunking")
	}
}

func TestPlanWholeArtifactFits(t *testing.T) {
	for _, budget := range []int{100, 0, -1} {
		plan := PlanChunks("hello world", budget)
		if plan.Count != 1 || len(plan.Chunks) != 1 {
			t.Fatalf("budget %d: count = %d, want 1", budget, plan.Count)
		}
		c := plan.Chunks[0]
		if c.Index != 0 || c.Offset != 0 || c.Len != len("hello world") {
			t.Fatalf("budget %d: span = %+v", budget, c)
		}
		if plan.OverBudget || plan.Truncated {
			t.Fatalf("budget %d: flags set on a fitting artifact", budget)
		}
	}
}

// The spans must tile the origin exactly: no gap loses content and no overlap
// shows a line to the panel twice.
func assertTiles(t *testing.T, origin string, plan ChunkPlan) {
	t.Helper()
	pos := 0
	for i, c := range plan.Chunks {
		if c.Index != i {
			t.Fatalf("span %d has index %d", i, c.Index)
		}
		if c.Offset != pos {
			t.Fatalf("span %d starts at %d, expected %d (gap or overlap)", i, c.Offset, pos)
		}
		if c.Len <= 0 {
			t.Fatalf("span %d is empty", i)
		}
		pos += c.Len
	}
	if !plan.Truncated && pos != len(origin) {
		t.Fatalf("spans cover %d of %d bytes", pos, len(origin))
	}
}

func TestPlanBreaksOnLineBoundaries(t *testing.T) {
	origin := "aaaa\nbbbb\ncccc\ndddd\n"
	plan := PlanChunks(origin, 10)
	assertTiles(t, origin, plan)
	if plan.OverBudget {
		t.Fatal("splittable lines must not report over_budget")
	}
	// Every span but possibly the last should end on a newline.
	for i, c := range plan.Chunks[:len(plan.Chunks)-1] {
		if origin[c.Offset+c.Len-1] != '\n' {
			t.Fatalf("span %d does not end on a newline", i)
		}
	}
}

func TestPlanIndivisibleLineTakesHardCut(t *testing.T) {
	origin := strings.Repeat("x", 50) // no newline anywhere
	plan := PlanChunks(origin, 10)
	assertTiles(t, origin, plan)
	if !plan.OverBudget {
		t.Fatal("an indivisible line longer than the budget must set over_budget")
	}
	for _, c := range plan.Chunks {
		if c.Len > 10 {
			t.Fatalf("hard cut produced a span of %d over a budget of 10", c.Len)
		}
	}
}

func TestPlanTruncatesRatherThanOverrun(t *testing.T) {
	// Far more content than MaxChunks spans of one byte each.
	origin := strings.Repeat("x", MaxChunks*4)
	plan := PlanChunks(origin, 1)
	if plan.Count != MaxChunks {
		t.Fatalf("count = %d, want the cap %d", plan.Count, MaxChunks)
	}
	if !plan.Truncated {
		t.Fatal("running out of slots must set truncated so the caller sees the gap")
	}
}

func TestAssemblySelectsWithinBudget(t *testing.T) {
	origin := "aaaa\nbbbb\ncccc\ndddd\n"
	plan := PlanChunks(origin, 5)
	// A budget big enough for everything selects everything and omits nothing.
	all := BuildAssembly(plan, len(origin)+10)
	if all.SelectedCount != plan.Count || all.OmittedCount != 0 {
		t.Fatalf("generous budget: %+v", all)
	}
	if all.UsedBytes != len(origin) {
		t.Fatalf("used %d of %d", all.UsedBytes, len(origin))
	}

	// A tight budget takes a prefix and records the rest as omitted, not lost.
	tight := BuildAssembly(plan, 6)
	if tight.SelectedCount+tight.OmittedCount != plan.Count {
		t.Fatalf("spans lost: %+v", tight)
	}
	if tight.UsedBytes > 6 {
		t.Fatalf("used %d over a budget of 6", tight.UsedBytes)
	}
}

func TestAssemblyFirstSpanOverBudget(t *testing.T) {
	plan := PlanChunks(strings.Repeat("x", 40), 0) // one span of 40
	a := BuildAssembly(plan, 10)
	if !a.OverBudget {
		t.Fatal("a first span larger than the synthesis budget must set over_budget")
	}
}

func TestAssemblyNoBudgetTakesEverything(t *testing.T) {
	origin := "aaaa\nbbbb\ncccc\n"
	plan := PlanChunks(origin, 5)
	a := BuildAssembly(plan, 0)
	if a.SelectedCount != plan.Count || a.OmittedCount != 0 {
		t.Fatalf("no budget must select every span: %+v", a)
	}
}
