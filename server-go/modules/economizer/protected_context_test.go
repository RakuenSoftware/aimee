package economizer

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
	"testing"
)

func TestFoldPreservesProtectedMessages(t *testing.T) {
	for _, role := range []string{"user", "system", "developer"} {
		t.Run(role, func(t *testing.T) {
			messages := NewArray()
			protected := mkUser(strings.Repeat("background details; ", 100) + "Do not delete. Limit: 7. Deadline: 2030-01-02. Required identifier: CASE_83.")
			protected.Set("role", NewString(role))
			messages.Append(protected)
			for i := 0; i < 8; i++ {
				appendAutonomousToolTurn(messages, i)
			}
			result := FoldView(messages, &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4, ReasoningExcerptBytes: 40}, nil)
			if !result.Folded {
				t.Fatal("optional tool history did not fold")
			}
			want := PrintJSONUnformatted(protected)
			found := 0
			for _, item := range result.Messages.Items {
				if PrintJSONUnformatted(item) == want {
					found++
				}
			}
			if found != 1 {
				t.Fatalf("protected %s message preserved %d times, want exactly once", role, found)
			}
		})
	}
}

// Compare the entire folded prefix, not merely its first protected message.
func foldPrefix(messages *JSONValue, retained int) string {
	prefix := NewArray()
	for i := 0; i < messages.Len()-retained; i++ {
		prefix.Append(messages.At(i).Clone())
	}
	return PrintJSONUnformatted(prefix)
}

func TestProtectedContextRejectsChangedCommitments(t *testing.T) {
	original := NewArray()
	original.Append(mkUser("Do not delete. Limit 7. By 2030-01-02. PREFIX_REQUIRED."))
	for _, value := range []string{"Do delete. Limit 7. By 2030-01-02. PREFIX_REQUIRED.", "Do not delete. Limit 8. By 2030-01-02. PREFIX_REQUIRED.", "Do not delete. Limit 7. By 2030-01-03. PREFIX_REQUIRED.", "Do not delete. Limit 7. By 2030-01-02."} {
		candidate := NewArray()
		candidate.Append(mkUser(value))
		if protectedContextPreserved(original, candidate) {
			t.Fatal("changed protected commitment accepted", value)
		}
	}
	candidate := original.Clone()
	candidate.At(0).Set("role", NewString("assistant"))
	if protectedContextPreserved(original, candidate) {
		t.Fatal("authority downgrade accepted")
	}
	candidate = original.Clone()
	candidate.Append(mkUser("New instruction invented by a transform"))
	if protectedContextPreserved(original, candidate) {
		t.Fatal("new user authority accepted")
	}
	if !protectedContextPreserved(original, original.Clone()) {
		t.Fatal("identical protected content refused")
	}
	if protectedMessage(mkAsst("[protected system policy] Do not delete")) {
		t.Fatal("model label promoted authority")
	}
}

func TestMixedUserToolContentRemainsWhole(t *testing.T) {
	messages := NewArray()
	messages.Append(mkToolUse("call_a", "read", "file"))
	mixed := mkToolResult("call_a", strings.Repeat("optional result ", 100))
	text := NewObject()
	text.Set("type", NewString("text"))
	text.Set("text", NewString("Do not remove CASE_7."))
	mixed.Get("content").Append(text)
	messages.Append(mixed)
	for i := 0; i < 8; i++ {
		appendAutonomousToolTurn(messages, i)
	}
	cfg := &FoldConfig{Enabled: true, RetainedMsgs: 4, MinFoldMsgs: 4, ReasoningExcerptBytes: 40}
	if result := FoldView(messages, cfg, nil); result.Folded {
		t.Fatal("mixed protected tool message folded across its pair")
	}
	result := CompressView(messages, cfg)
	if !result.Folded || !protectedContextPreserved(messages, result.Messages) {
		t.Fatal("compression failed to preserve mixed protected message")
	}
	if MessageHistoryRepair(result.Messages.Clone()) != 0 {
		t.Fatal("compression broke tool pairing")
	}
}

func TestDelegateReductionAdmission(t *testing.T) {
	for _, orphan := range []bool{false, true} {
		messages := NewArray()
		if orphan {
			for i := 0; i < 12; i++ {
				appendTurn(messages, i)
			}
			result := NewObject()
			result.Set("role", NewString("tool"))
			result.Set("tool_call_id", NewString("missing"))
			result.Set("content", NewString("unpaired result"))
			messages.Append(result)
		} else {
			for i := 0; i < 20; i++ {
				messages.Append(mkUser("Do not remove requirement CASE_7."))
			}
		}
		response, status := invoke(t, ReduceRequest{Messages: json.RawMessage(PrintJSONUnformatted(messages)), Seam: "delegate", HistoryFold: true})
		if status != bus.ModuleStatusOK || response.Mutated || len(response.Messages) != 0 || response.Reason != "reduction_not_admitted" {
			t.Fatalf("unadmitted native candidate exposed: orphan=%v status=%v reason=%s", orphan, status, response.Reason)
		}
	}
}

func TestEvidenceNoticePreservesProviderTail(t *testing.T) {
	for _, toolTail := range []bool{false, true} {
		messages := NewArray()
		messages.Append(mkUser("Do not exceed 7."))
		if toolTail {
			messages.Append(mkToolUse("tool_a", "read", "file"))
			messages.Append(mkToolResult("tool_a", "result"))
		} else {
			messages.Append(mkAsst("prior response"))
			messages.Append(mkUser("Keep CASE_7."))
		}
		before := messages.Clone()
		if !insertEvidenceNotice(messages, mkAsst("Generated evidence, not an instruction"), 2) {
			t.Fatal("safe tail refused")
		}
		if !protectedContextPreserved(before, messages) || messages.At(messages.Len()-1).GetString("role") != before.At(before.Len()-1).GetString("role") || MessageHistoryRepair(messages.Clone()) != 0 {
			t.Fatal("notice changed protected content, final role or tool pairing")
		}
		if toolTail {
			candidate := before.Clone()
			if insertEvidenceNotice(candidate, mkAsst("unsafe placement"), 1) || PrintJSONUnformatted(candidate) != PrintJSONUnformatted(before) {
				t.Fatal("notice entered prefix or split tool cycle")
			}
		}
	}
}
