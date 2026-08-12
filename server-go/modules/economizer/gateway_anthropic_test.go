package economizer

import (
	"strings"
	"testing"
)

// The gateway seam folds Anthropic-shaped traffic now that it carries a freeze
// boundary. These tests cover the properties that change made a live dependency,
// on the shape a Claude client actually sends: content-block arrays carrying
// tool_use / tool_result rather than plain strings.
//
// They exist because the box this was built against has no Claude credentials, so
// the claude path cannot be exercised end to end there. That is a real gap and
// these do not close it: they pin the mechanics (structure, conservation, freeze
// stability) so that what remains unverified is the wire contract with Anthropic,
// not the reducer.

// anthropicFixture builds a conversation in Anthropic's content-block shape, long
// enough to fold, with tool_use/tool_result pairs and identifiers worth conserving.
func anthropicFixture(turns int) *JSONValue {
	m := NewArray()
	m.Append(mkUser("open incident INC-91024 on /srv/api/gateway.c"))
	for i := 0; i < turns; i++ {
		id := "toolu_gw_" + string(rune('a'+i%26))
		m.Append(mkToolUse(id, "read_log", "/var/log/api/"+id+".log"))
		m.Append(mkToolResult(id, "worker pid 77"+string(rune('0'+i%10))+" retry window 4s"))
		m.Append(mkAsst("checked " + id))
		m.Append(mkUser("continue " + id))
	}
	return m
}

// A fold that splits between a tool_use and its tool_result orphans the pair, and
// every provider rejects that. Anthropic is the shape where it is easiest to get
// wrong, because the pairing lives inside the content array rather than in
// separate messages.
func TestGatewayFoldKeepsAnthropicToolPairsIntact(t *testing.T) {
	cfg := &ReduceConfig{
		GatewaySeam: true,
		HistoryFold: true,
		Fold: FoldConfig{
			Closet:           ClosetConfig{Enabled: true},
			CompactHeadBytes: 40,
		},
	}
	m := anthropicFixture(8)
	out := Reduce(m, "sys", SeamGateway, cfg, &ReduceState{Recall: NewRecallIndex()})
	if !out.Mutated || out.Messages == nil {
		t.Fatalf("gateway fold did not engage on anthropic-shaped input: %+v", out)
	}

	// Every surviving tool_result must still have its tool_use ahead of it.
	open := map[string]bool{}
	for i := 0; i < out.Messages.Len(); i++ {
		content := out.Messages.At(i).Get("content")
		if !content.IsArray() {
			continue
		}
		for j := 0; j < content.Len(); j++ {
			b := content.At(j)
			switch b.GetString("type") {
			case "tool_use":
				open[b.GetString("id")] = true
			case "tool_result":
				if !open[b.GetString("tool_use_id")] {
					t.Fatalf("orphaned tool_result %q survived the fold",
						b.GetString("tool_use_id"))
				}
			}
		}
	}
}

// Folding is only allowed to be lossy about PROSE. An identifier the user can ask
// about later has to come back verbatim, or the reduction traded tokens for the
// answer being wrong -- which is the one trade this module may never make.
func TestGatewayFoldConservesAnthropicIdentifiers(t *testing.T) {
	cfg := &ReduceConfig{
		GatewaySeam: true,
		HistoryFold: true,
		Fold: FoldConfig{
			Closet:           ClosetConfig{Enabled: true},
			CompactHeadBytes: 40,
		},
	}
	m := anthropicFixture(8)
	out := Reduce(m, "sys", SeamGateway, cfg, &ReduceState{Recall: NewRecallIndex()})
	if !out.Mutated || out.Messages == nil {
		t.Fatal("gateway fold did not engage")
	}
	got := PrintJSONUnformatted(out.Messages)
	// The opening message names a path and an incident id; both are folded away as
	// prose and must survive in the closet.
	for _, want := range []string{"/srv/api/gateway.c", "INC-91024"} {
		if !strings.Contains(got, want) {
			t.Errorf("identifier %q was lost in the fold", want)
		}
	}
}

// THE PROPERTY THE CLAUDE CHANGE RESTS ON. Anthropic was excluded from the gateway
// because mutating its wire "would bust the cached prefix" -- true of a gateway
// that re-folded from cold every turn. With a persisted freeze the prefix must move
// at most once and then hold byte-identical, because a prefix that moves every turn
// costs a cache read every turn and folding would be worse than doing nothing.
func TestGatewayFreezeHoldsAcrossAnthropicTurns(t *testing.T) {
	cfg := &ReduceConfig{
		GatewaySeam: true,
		HistoryFold: true,
		Fold: FoldConfig{
			Closet:           ClosetConfig{Enabled: true},
			CompactHeadBytes: 40,
		},
	}
	// One state carried across turns, exactly as the seam persists and reloads it.
	st := &ReduceState{Freeze: FoldFreeze{TailCapMsgs: 16}, Recall: NewRecallIndex()}

	prevPrefix := ""
	prevEpochs := -1
	reuses := 0

	for turn := 0; turn < 10; turn++ {
		m := anthropicFixture(8 + turn) // the conversation grows, as a real one does
		st.Turn = turn
		out := Reduce(m, "sys", SeamGateway, cfg, st)
		st.Reduced = false

		if !out.Mutated || out.Messages == nil {
			continue
		}
		prefix := PrintJSONUnformatted(out.Messages.At(0))
		if prevPrefix != "" && out.Epochs == prevEpochs {
			if prefix != prevPrefix {
				t.Fatalf("turn %d: the frozen prefix changed without an epoch advance; "+
					"an anthropic upstream would miss its cache on every turn", turn)
			}
			reuses++
		}
		prevPrefix = prefix
		prevEpochs = out.Epochs
	}

	if reuses == 0 {
		t.Fatal("the freeze never held across turns: folding here would cost a cache " +
			"read every turn, which is the exact reason anthropic used to be excluded")
	}
}

// State has to survive the round trip the seam actually performs: serialize, store,
// reload. If the freeze boundary does not survive it, every gateway turn starts cold
// and the freeze is decorative.
func TestGatewayFreezeSurvivesStateRoundTrip(t *testing.T) {
	cfg := &ReduceConfig{
		GatewaySeam: true,
		HistoryFold: true,
		Fold: FoldConfig{
			Closet:           ClosetConfig{Enabled: true},
			CompactHeadBytes: 40,
		},
	}
	st := &ReduceState{Freeze: FoldFreeze{TailCapMsgs: 16}, Recall: NewRecallIndex()}
	m := anthropicFixture(8)
	first := Reduce(m, "sys", SeamGateway, cfg, st)
	if !first.Mutated || first.Messages == nil {
		t.Fatal("first gateway fold did not engage")
	}
	firstPrefix := PrintJSONUnformatted(first.Messages.At(0))

	// Round-trip through exactly what the seam stores.
	blob, ok := SerializeState(st)
	if !ok || blob == "" {
		t.Fatal("the seam would have nothing to persist")
	}
	restored := &ReduceState{Recall: NewRecallIndex()}
	if err := RestoreState(restored, blob); err != nil {
		t.Fatalf("state did not survive the round trip: %v", err)
	}
	restored.Turn = 1

	second := Reduce(anthropicFixture(9), "sys", SeamGateway, cfg, restored)
	if !second.Mutated || second.Messages == nil {
		t.Fatal("second gateway fold did not engage after reload")
	}
	if second.Epochs == first.Epochs {
		if got := PrintJSONUnformatted(second.Messages.At(0)); got != firstPrefix {
			t.Error("the frozen prefix changed across a state reload without an epoch " +
				"advance; the persisted boundary is not being honoured")
		}
	}
}
