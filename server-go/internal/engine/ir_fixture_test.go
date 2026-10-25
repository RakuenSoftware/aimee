package engine

import canonical "github.com/JBailes/aimee/server-go/ir"

// Scripted agents declare a text transport at their fixture boundary.
func testCanonicalDelegate(r DelegateResult) DelegateResult {
	r.Output = canonical.TextResponse(r.Response)
	return r
}
func testCanonicalGroup(r DelegateGroupResult) DelegateGroupResult {
	r.Output = canonical.TextResponse(r.Response)
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
