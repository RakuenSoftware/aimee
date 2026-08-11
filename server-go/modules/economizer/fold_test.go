package economizer

import (
	"fmt"
	"strings"
	"testing"
)

// DIFFERENTIAL test against the real C fold.
//
// The golden strings are the VERBATIM cJSON_PrintUnformatted output of
// context_fold_view / context_compress_view for these fixtures, captured by
// compiling src/modules/economizer/context_fold.c against a throwaway harness.
// The C in this tree already carries the #2552 tail-note fix, so the compress
// golden pins that placement too.
//
// This is the test that makes the fold port trustworthy: the folded prefix is
// the cache key, so equality has to be on BYTES, not on shape.

func mkUser(text string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("user"))
	m.Set("content", NewString(text))
	return m
}

func mkAsst(text string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("assistant"))
	m.Set("content", NewString(text))
	return m
}

// The tool_use input is built zeta-then-alpha ON PURPOSE: it is what proves the
// skeleton preserves cJSON key order. Go's encoding/json would emit alpha first.
func mkToolUse(id, name, argval string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("assistant"))
	c := NewArray()
	b := NewObject()
	b.Set("type", NewString("tool_use"))
	b.Set("id", NewString(id))
	b.Set("name", NewString(name))
	in := NewObject()
	in.Set("zeta", NewString(argval))
	in.Set("alpha", NewNumber(3))
	b.Set("input", in)
	c.Append(b)
	m.Set("content", c)
	return m
}

func mkToolResult(id, body string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("user"))
	c := NewArray()
	b := NewObject()
	b.Set("type", NewString("tool_result"))
	b.Set("tool_use_id", NewString(id))
	b.Set("content", NewString(body))
	c.Append(b)
	m.Set("content", c)
	return m
}

func foldFixture() *JSONValue {
	m := NewArray()
	m.Append(mkUser("start job 7fd5835b-1a2b-4c3d-8e9f-0123456789ab"))
	m.Append(mkToolUse("toolu_1", "read", "/etc/hosts"))
	m.Append(mkToolResult("toolu_1", "ok; bound on port=3002"))
	m.Append(mkAsst("read complete"))
	m.Append(mkUser("next please"))
	m.Append(mkToolUse("toolu_2", "bash", "ls -la"))
	m.Append(mkToolResult("toolu_2", "file listing at /var/lib/aimee/x.log"))
	m.Append(mkAsst("[verdict] listing done"))
	m.Append(mkUser("keep going"))
	m.Append(mkAsst("sure"))
	m.Append(mkUser("almost there"))
	m.Append(mkAsst("done"))
	return m
}

const foldGolden = `[{"role":"user","content":"[folded 8 earlier message(s); skeleton below — exact identifiers are conserved in the Coordinate Closet, full bodies remain in history]\n\nuser: start job 7fd5835b-1a2b-4c3d-8e9f-012345…\n  $ read {\"zeta\":\"/etc/hosts\",\"alpha\":3}\n    → ok; bound on port=3002 (22 bytes)\nassistant/wip: read complete\nuser: next please\n  $ bash {\"zeta\":\"ls -la\",\"alpha\":3}\n    → file listing at /var/lib/aimee/x.log (36 bytes)\nassistant/verdict: [verdict] listing done\n\nCoordinate Closet (conserved from folded turns):\n  /etc/hosts ⟦path⟧\n  /var/lib/aimee/x.log ⟦path⟧\n  3002 ⟦port⟧\n  7fd5835b-1a2b-4c3d-8e9f-0123456789ab ⟦uuid⟧\n"},{"role":"assistant","content":"Understood — continuing from the folded summary above."},{"role":"user","content":"keep going"},{"role":"assistant","content":"sure"},{"role":"user","content":"almost there"},{"role":"assistant","content":"done"}]`

func TestFoldMatchesC(t *testing.T) {
	cfg := &FoldConfig{
		Enabled:               true,
		RetainedMsgs:          4,
		MinFoldMsgs:           4,
		ReasoningExcerptBytes: 40,
		RegisterEnabled:       true,
		Closet:                ClosetConfig{Enabled: true},
	}
	r := FoldView(foldFixture(), cfg, nil)
	if !r.Folded || r.Messages == nil {
		t.Fatal("fixture did not fold")
	}
	if got := PrintJSONUnformatted(r.Messages); got != foldGolden {
		t.Errorf("fold output drifted from C:\n got: %s\nwant: %s", got, foldGolden)
	}
	if r.FoldedMsgs != 8 || r.RetainedMsgs != 4 || r.ClosetEvict != EvictNone {
		t.Errorf("meta: folded=%d retained=%d evict=%v, want 8/4/EvictNone",
			r.FoldedMsgs, r.RetainedMsgs, r.ClosetEvict)
	}
}

// The fold must never mutate its input.
func TestFoldDoesNotMutateInput(t *testing.T) {
	m := foldFixture()
	before := PrintJSONUnformatted(m)
	cfg := &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4,
		ReasoningExcerptBytes: 40, Closet: ClosetConfig{Enabled: true}}
	FoldView(m, cfg, nil)
	if after := PrintJSONUnformatted(m); after != before {
		t.Error("FoldView mutated its input")
	}
}

func compressFixture() *JSONValue {
	t := NewArray()
	t.Append(mkUser("do the task"))
	for k := 0; k < 6; k++ {
		id := fmt.Sprintf("call_%02d", k)
		a := NewObject()
		a.Set("role", NewString("assistant"))
		tcs := NewArray()
		tc := NewObject()
		tc.Set("id", NewString(id))
		tc.Set("type", NewString("function"))
		fn := NewObject()
		fn.Set("name", NewString("read_file"))
		fn.Set("arguments", NewString("{}"))
		tc.Set("function", fn)
		tcs.Append(tc)
		a.Set("tool_calls", tcs)
		t.Append(a)

		// Mirrors the C harness byte-for-byte: repeat the filler while
		// pos+48 < 900-96, then append the tail.
		var body strings.Builder
		for body.Len()+48 < 900-96 {
			body.WriteString("filler output bytes here; ")
		}
		fmt.Fprintf(&body, "tail at /work/src/stage_%d.c done", k)

		tr := NewObject()
		tr.Set("role", NewString("tool"))
		tr.Set("tool_call_id", NewString(id))
		tr.Set("content", NewString(body.String()))
		t.Append(tr)
	}
	return t
}

func TestCompressMatchesC(t *testing.T) {
	cfg := &FoldConfig{
		Enabled:               true,
		RetainedMsgs:          4,
		ReasoningExcerptBytes: 120,
		CompactHeadBytes:      40,
		Closet:                ClosetConfig{Enabled: true},
	}
	r := CompressView(compressFixture(), cfg)
	if !r.Folded || r.Messages == nil {
		t.Fatal("fixture did not compress")
	}
	if r.FoldedMsgs != 4 || r.ClosetEvict != EvictNone {
		t.Errorf("meta: compressed=%d evict=%v, want 4/EvictNone", r.FoldedMsgs, r.ClosetEvict)
	}

	n := r.Messages.Len()
	last := r.Messages.At(n - 1)
	// #2552: the conserving note is APPENDED, never prepended.
	if last.GetString("role") != "user" ||
		!strings.Contains(last.GetString("content"), "Coordinate Closet") {
		t.Fatalf("closet note is not the final message: %s", PrintJSONUnformatted(last))
	}
	if strings.Contains(r.Messages.At(0).GetString("content"), "Coordinate Closet") {
		t.Error("closet note must not sit at the head — that is what broke the freeze")
	}
	// The original first turn is still first.
	if r.Messages.At(0).GetString("content") != "do the task" {
		t.Error("compress reordered the transcript head")
	}
	// Buried identifiers past the excerpt window survive via the closet.
	for k := 0; k < 4; k++ {
		want := fmt.Sprintf("/work/src/stage_%d.c", k)
		if !strings.Contains(last.GetString("content"), want) {
			t.Errorf("closet lost %s", want)
		}
	}
	// The retained tail kept its full bodies.
	if body := r.Messages.At(n - 2).GetString("content"); !strings.Contains(body, "stage_5.c") ||
		strings.Contains(body, "bytes omitted") {
		t.Error("retained tail should not have been compressed")
	}
}

// The freeze holds a boundary across turns and re-epochs when the tail outgrows
// its cap — the property the prompt cache depends on.
func TestFoldFreezeReusesBoundary(t *testing.T) {
	cfg := &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4,
		ReasoningExcerptBytes: 40, Closet: ClosetConfig{Enabled: true}}
	m := foldFixture()
	fz := &FoldFreeze{TailCapMsgs: 12}

	r1 := FoldView(m, cfg, fz)
	if !r1.Folded || r1.ReusedBoundary {
		t.Fatal("first fold should pin, not reuse")
	}
	pinned := fz.FrozenSplit
	prefix1 := PrintJSONUnformatted(r1.Messages.At(0))

	// Append a round: prefix unchanged and tail within cap -> reuse, and the
	// emitted prefix must be BYTE-identical.
	m.Append(mkUser("another request"))
	m.Append(mkAsst("another answer"))
	r2 := FoldView(m, cfg, fz)
	if !r2.ReusedBoundary || fz.FrozenSplit != pinned {
		t.Fatalf("second fold should reuse the pinned boundary (reused=%v split=%d/%d)",
			r2.ReusedBoundary, fz.FrozenSplit, pinned)
	}
	if got := PrintJSONUnformatted(r2.Messages.At(0)); got != prefix1 {
		t.Error("reused boundary must yield a byte-identical prefix")
	}

	// Mutating the folded prefix must force an epoch, not a false reuse.
	epochsBefore := fz.Epochs
	m.At(0).Set("content", NewString("MUTATED prefix content"))
	m.Append(mkUser("carry on"))
	m.Append(mkAsst("sure"))
	r3 := FoldView(m, cfg, fz)
	if r3.ReusedBoundary || fz.Epochs != epochsBefore+1 {
		t.Errorf("prefix mutation must epoch: reused=%v epochs=%d->%d",
			r3.ReusedBoundary, epochsBefore, fz.Epochs)
	}
}

// A transcript with no clean user-turn boundary must not fold at all.
func TestFoldNoCleanBoundary(t *testing.T) {
	m := NewArray()
	m.Append(mkUser("start"))
	for k := 0; k < 10; k++ {
		id := fmt.Sprintf("toolu_%02d", k)
		m.Append(mkToolUse(id, "read", "/x"))
		m.Append(mkToolResult(id, "result body"))
	}
	cfg := &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4,
		ReasoningExcerptBytes: 40, Closet: ClosetConfig{Enabled: true}}
	if r := FoldView(m, cfg, nil); r.Folded {
		t.Error("a tool loop has no clean boundary and must not fold")
	}
}

func TestFoldDisabledAndTooShort(t *testing.T) {
	cfg := &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4}
	if r := FoldView(foldFixture(), &FoldConfig{Enabled: false}, nil); r.Folded {
		t.Error("disabled fold must not fold")
	}
	short := NewArray()
	short.Append(mkUser("hi"))
	short.Append(mkAsst("hello"))
	if r := FoldView(short, cfg, nil); r.Folded {
		t.Error("too-short transcript must not fold")
	}
}

// Identical input must fold to identical bytes — the determinism the freeze and
// the cache both rest on.
func TestFoldDeterministic(t *testing.T) {
	cfg := &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4,
		ReasoningExcerptBytes: 40, Closet: ClosetConfig{Enabled: true}}
	a := FoldView(foldFixture(), cfg, nil)
	b := FoldView(foldFixture(), cfg, nil)
	if PrintJSONUnformatted(a.Messages) != PrintJSONUnformatted(b.Messages) {
		t.Error("identical input must produce identical folded bytes")
	}
}

// Multi-byte characters must not be split by the excerpt cap.
func TestFoldExcerptUTF8Boundary(t *testing.T) {
	var b strings.Builder
	appendExcerpt(&b, strings.Repeat("é", 20), 5) // é is 2 bytes; 5 lands mid-char
	got := b.String()
	trimmed := strings.TrimSuffix(got, "…")
	if len(trimmed)%2 != 0 {
		t.Errorf("excerpt split a multibyte character: %q", got)
	}
	if !strings.HasSuffix(got, "…") {
		t.Errorf("truncation must be marked: %q", got)
	}
}
