package ir

import (
	"encoding/json"
	"testing"
)

func TestRepairRequestRetainsCanonicalTaskAndAssistantBlocks(t *testing.T) {
	input := TaskRequest("reviewer", "Review the original task")
	temperature := 0.17
	input.Temperature = &temperature
	input.Metadata = json.RawMessage(`{"exact":9007199254740993123456789}`)
	input.Messages[0].Blocks[0].Context.RevisionScope = "original-task"
	output := TextResponse("first block")
	second := "second block"
	output.Blocks = append(output.Blocks, ResponseBlock{Block: Block{Type: "text", Text: &second, Context: &Context{Origin: 7, Authority: 1, Trust: 2, ModelVisible: true}}})
	output.Blocks[0].Raw = json.RawMessage(`{"native":9007199254740993123456789}`)
	repaired, err := RepairRequest(input, output, "Repair serialization only")
	if err != nil {
		t.Fatal(err)
	}
	if len(repaired.Messages) != 3 || repaired.Messages[1].Role != "assistant" || len(repaired.Messages[1].Blocks) != 2 {
		t.Fatalf("lost assistant blocks: %+v", repaired.Messages)
	}
	if *repaired.Messages[0].Blocks[0].Text != "Review the original task" || repaired.Messages[0].Blocks[0].Context.RevisionScope != "original-task" || *repaired.Temperature != temperature || string(repaired.Metadata) != string(input.Metadata) {
		t.Fatal("task context, sampling or opaque metadata changed")
	}
	if repaired.Messages[2].Blocks[0].Context.Origin != 1 || repaired.Messages[2].Blocks[0].Context.Authority != 3 {
		t.Fatal("repair instruction lacks separate host provenance")
	}
	*repaired.Messages[0].Blocks[0].Text = "changed"
	*repaired.Messages[1].Blocks[0].Text = "changed"
	if *input.Messages[0].Blocks[0].Text != "Review the original task" || *output.Blocks[0].Text != "first block" {
		t.Fatal("repair aliases source ownership")
	}
	if len(input.Messages) != 1 || string(output.Blocks[0].Raw) != `{"native":9007199254740993123456789}` {
		t.Fatal("repair mutated original task or native output replay")
	}
}

func TestRepairRequestRefusesIncompleteResponse(t *testing.T) {
	output := TextResponse("partial")
	output.StopReason = StopMaxTokens
	if _, err := RepairRequest(TaskRequest("", "task"), output, "repair"); err == nil {
		t.Fatal("incomplete output admitted as a repairable artifact")
	}
}
