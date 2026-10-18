package economizer

import "github.com/JBailes/aimee/server-go/ir"

const cancelMsg = "[Tool call was cancelled or timed out]"

// Repair canonical tool cycles without native protocol detection, argument
// fabrication or flat call limits. Refusal leaves the original untouched.
func canonicalHistory(messages *JSONValue) bool {
	if !messages.IsArray() {
		return false
	}
	_, err := ir.DecodeMessages([]byte(PrintJSONUnformatted(messages)))
	return err == nil
}
func MessageHistoryRepair(messages *JSONValue) int {
	if !canonicalHistory(messages) {
		return -1
	}
	return repairCanonicalHistory(messages)
}

// Pair against the current cycle rather than a global ID set: providers may
// reuse an opaque ID in a later response. Preserve every non-tool block.
func repairCanonicalHistory(messages *JSONValue) int {
	rebuilt := NewArray()
	pending := []string{}
	repairs := 0
	flush := func() {
		for _, id := range pending {
			if id == "" {
				continue
			}
			block := NewObject()
			block.Set("type", NewString("tool_result"))
			block.Set("tool_id", NewString(id))
			block.Set("content", NewString(cancelMsg))
			block.Set("is_error", &JSONValue{Kind: JSONTrue, Bool: true})
			blocks := NewArray()
			blocks.Append(block)
			message := NewObject()
			message.Set("role", NewString("tool"))
			message.Set("blocks", blocks)
			rebuilt.Append(message)
			repairs++
		}
		pending = pending[:0]
	}
	for _, message := range messages.Items {
		blocks := message.Get("blocks")
		if !blocks.IsArray() {
			return -1
		}
		hasResults := false
		for _, block := range blocks.Items {
			hasResults = hasResults || block.GetString("type") == "tool_result"
		}
		if !hasResults {
			flush()
		}
		copy := message.Clone()
		kept := NewArray()
		for _, block := range copy.Get("blocks").Items {
			if block.GetString("type") == "tool_result" {
				matched := false
				id := block.GetString("tool_id")
				for i, call := range pending {
					if id != "" && call == id {
						pending[i] = ""
						matched = true
						break
					}
				}
				if !matched {
					repairs++
					continue
				}
			}
			kept.Append(block)
		}
		copy.Set("blocks", kept)
		for _, block := range blocks.Items {
			if block.GetString("type") == "tool_use" {
				id := block.GetString("tool_id")
				if id == "" {
					return -1
				}
				for _, previous := range pending {
					if previous == id {
						return -1
					}
				}
				pending = append(pending, id)
			}
		}
		if kept.Len() > 0 || blocks.Len() == 0 {
			rebuilt.Append(copy)
		}
	}
	flush()
	if repairs > 0 {
		messages.Items = rebuilt.Items
	}
	return repairs
}
