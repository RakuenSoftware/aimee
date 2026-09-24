package economizer

// Protected classes come from the structured message role, never labels in
// model/tool prose. Preserve entire messages rather than guessing which words
// express negation, numerical limits, deadlines or identifiers.
func protectedMessage(message *JSONValue) bool {
	if message == nil {
		return false
	}
	switch message.GetString("role") {
	case "system", "developer":
		return true
	case "user":
		content := message.Get("content")
		if content.IsArray() && content.Len() > 0 {
			for _, block := range content.Items {
				if block.GetString("type") != "tool_result" {
					return true
				}
			}
			return false
		}
		return true
	}
	return false
}

func protectedContextPreserved(original, candidate *JSONValue) bool {
	if !original.IsArray() || !candidate.IsArray() {
		return false
	}
	var before, after []string
	for _, message := range original.Items {
		if protectedMessage(message) {
			before = append(before, PrintJSONUnformatted(message))
		}
	}
	for _, message := range candidate.Items {
		if protectedMessage(message) {
			after = append(after, PrintJSONUnformatted(message))
		}
	}
	if len(before) != len(after) {
		return false
	}
	for i := range before {
		if before[i] != after[i] {
			return false
		}
	}
	return true
}
