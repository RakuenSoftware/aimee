package economizer

import (
	"strings"
	"testing"
)

// The C original (agent_compress_tool_result) has NO tests and, as of this
// commit, NO callers — see the note on CompressToolResult. These are therefore
// written against its documented contract rather than ported from a C suite,
// and they are what make the ported behaviour checkable at all.

func eagerCfg() EagerConfig {
	return EagerConfig{
		Compact: CompactConfig{Enabled: true, Threshold: 64, HeadBytes: 20, TailBytes: 20},
		Closet:  ClosetConfig{Enabled: true, BudgetBytes: 2048, MaxRatioPct: 100},
	}
}

func TestEagerEmptyRaw(t *testing.T) {
	if got := CompressToolResult("", "", eagerCfg()); got.Body != "" || got.Compacted {
		t.Errorf("empty raw must produce an empty, uncompacted result: %+v", got)
	}
}

func TestEagerPassThroughWhenSmall(t *testing.T) {
	raw := "small result"
	got := CompressToolResult(raw, "", eagerCfg())
	if got.Body != raw || got.Compacted {
		t.Errorf("below threshold must pass through unchanged: %+v", got)
	}
}

// The closet nominates from the PRE-truncation raw, so an identifier that the
// shrink discards still survives. This is the whole reason the seam exists.
func TestEagerConservesIdentifiersDroppedByTheShrink(t *testing.T) {
	// Put the identifier in the middle, where head+tail truncation will drop it.
	raw := strings.Repeat("a", 200) +
		" /home/u/src/buried.c handle:deadbeef " +
		strings.Repeat("b", 200)
	got := CompressToolResult(raw, "", eagerCfg())
	if !got.Compacted {
		t.Fatalf("fixture should have compacted: %q", got.Body)
	}
	if strings.Contains(got.Body[:strings.Index(got.Body, "\n")+1], "buried.c") {
		t.Skip("fixture no longer buries the identifier; adjust the padding")
	}
	for _, want := range []string{"/home/u/src/buried.c", "handle:deadbeef", "Coordinate Closet"} {
		if !strings.Contains(got.Body, want) {
			t.Errorf("identifier %q was lost by the shrink and not conserved", want)
		}
	}
}

// No closet is rendered when nothing shrank: there is nothing to conserve
// against, and appending one would only add bytes.
func TestEagerNoClosetWhenNotCompacted(t *testing.T) {
	raw := "short /home/u/src/x.c body"
	got := CompressToolResult(raw, "", eagerCfg())
	if got.Compacted {
		t.Fatal("fixture should not compact")
	}
	if strings.Contains(got.Body, "Coordinate Closet") {
		t.Error("closet must not be appended when nothing was compacted")
	}
}

func TestEagerClosetDisabled(t *testing.T) {
	cfg := eagerCfg()
	cfg.Closet.Enabled = false
	raw := strings.Repeat("a", 200) + " /home/u/src/buried.c " + strings.Repeat("b", 200)
	got := CompressToolResult(raw, "", cfg)
	if strings.Contains(got.Body, "Coordinate Closet") {
		t.Error("closet disabled but still rendered")
	}
}

// The hard cap is per-seam policy and must hold regardless of what the shrink
// produced, with the closet counted inside it.
func TestEagerHardCapIncludesCloset(t *testing.T) {
	cfg := eagerCfg()
	cfg.ToolOutputCap = 300
	raw := strings.Repeat("a", 200) +
		" /home/u/src/one.c /home/u/src/two.c handle:abc123 " +
		strings.Repeat("b", 2000)
	got := CompressToolResult(raw, "", cfg)
	if len(got.Body) > 300 {
		t.Errorf("result %d bytes exceeds the resolved cap of 300", len(got.Body))
	}
}

// A cap so small there is no room for a closet must still produce a bounded
// body rather than failing.
func TestEagerTinyCapStillBounded(t *testing.T) {
	cfg := eagerCfg()
	cfg.ToolOutputCap = 100
	raw := strings.Repeat("a", 100) + " /home/u/src/x.c " + strings.Repeat("b", 2000)
	got := CompressToolResult(raw, "", cfg)
	if len(got.Body) > 100 {
		t.Errorf("result %d bytes exceeds the tiny cap of 100", len(got.Body))
	}
}

func TestToolOutputCapClamp(t *testing.T) {
	cases := []struct{ in, want int }{
		{0, AgentToolOutputMax},
		{-1, AgentToolOutputMax},
		{1024, 1024},
		{AgentToolOutputRawMax + 1, AgentToolOutputRawMax},
	}
	for _, c := range cases {
		if got := ToolOutputCapClamp(c.in); got != c.want {
			t.Errorf("ToolOutputCapClamp(%d) = %d, want %d", c.in, got, c.want)
		}
	}
}

// Per-tool overrides reach the shared core through this seam.
func TestEagerPerToolDisable(t *testing.T) {
	cfg := eagerCfg()
	cfg.Compact.PerTool = []PerToolCompact{{Tool: "read_file", Threshold: -1}}
	raw := strings.Repeat("a", 500)
	if got := CompressToolResult(raw, "read_file", cfg); got.Body != raw || got.Compacted {
		t.Error("per-tool -1 must disable compaction for that tool")
	}
	if got := CompressToolResult(raw, "other", cfg); !got.Compacted {
		t.Error("per-tool override must not leak to other tools")
	}
}
