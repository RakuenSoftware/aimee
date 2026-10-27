package delegate

import canonical "github.com/JBailes/aimee/server-go/ir"

func testCanonicalInvocation(r InvocationResult) InvocationResult {
	r.History = []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{r.Output.Blocks[0].Block}}}
	return r
}

// Test assertions inspect the canonical task message rather than a prompt DTO.
func fixtureInputText(input *canonical.Request) string {
	if input == nil {
		return ""
	}
	text := ""
	for _, message := range input.Messages {
		if message.Role != "user" {
			continue
		}
		for _, block := range message.Blocks {
			if block.Type == "text" && block.Text != nil {
				text += *block.Text
			}
		}
	}
	return text
}
