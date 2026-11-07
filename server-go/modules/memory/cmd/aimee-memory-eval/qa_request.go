package main

import canonical "github.com/JBailes/aimee/server-go/ir"

func qaBlock(text string, origin, authority, trust int) canonical.Block {
	return canonical.Block{Type: "text", Text: &text, Context: &canonical.Context{
		Origin: origin, Authority: authority, Trust: trust, Sensitivity: 1, ModelVisible: true}}
}

// These constructors own new evaluation tasks. The transport receives only IR;
// The evaluator is a caller of the completion owner, so its instructions claim
// caller authority; memory and candidate artifacts retain evidence authority.
func qaRequest(system string, tokens int, temperature float64, blocks ...canonical.Block) *canonical.Request {
	return &canonical.Request{Version: 1, SourceWire: "unspecified", CachePolicy: "native",
		System:   []canonical.Block{qaBlock(system, 3, 2, 2)},
		Messages: []canonical.Message{{Role: "user", Blocks: blocks}},
		Tools:    []canonical.Tool{}, Stops: []string{}, ToolChoice: canonical.ToolChoice{Mode: "none"},
		MaxTokens: &tokens, Temperature: &temperature}
}

func answerEvaluationRequest(question, memory string) *canonical.Request {
	return qaRequest(answerSystem, 256, .3,
		qaBlock("Question: "+question, 3, 2, 2),
		qaBlock("\n\nRetrieved memory context:\n"+memory, 6, 1, 2),
		qaBlock("\nFinal answer:", 3, 2, 2))
}

// Candidate is a declared QA artifact projected from a complete canonical answer,
// not a replacement for any live conversation or model response.
func judgeEvaluationRequest(question, gold, candidate string, answerable *bool) *canonical.Request {
	blocks := []canonical.Block{qaBlock("Question: "+question, 3, 1, 2)}
	footer := "\n\nReturn JSON only."
	if answerable != nil && !*answerable {
		blocks = append(blocks, qaBlock("\nUnanswerable from supplied history. Score 1 only for explicit abstention without asserting an answer.", 3, 2, 2))
		footer = "\nReturn JSON only."
	} else {
		blocks = append(blocks, qaBlock("\nGold answer: "+gold, 3, 1, 2))
	}
	blocks = append(blocks, qaBlock("\nCandidate answer: "+candidate, 7, 1, 2),
		qaBlock(footer, 3, 2, 2))
	return qaRequest(judgeSystem, 64, 0, blocks...)
}

// A read-only token estimate keeps the existing, explicitly labelled QA metric.
func qaRequestEstimatedTokens(request *canonical.Request) int {
	count := 0
	add := func(blocks []canonical.Block) {
		for _, block := range blocks {
			if block.Text != nil {
				count += len(*block.Text)
			}
		}
	}
	add(request.System)
	systemTokens := (count + 3) / 4
	count = 0
	for _, message := range request.Messages {
		add(message.Blocks)
	}
	return systemTokens + (count+3)/4
}
