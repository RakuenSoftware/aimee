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

// Insert generated evidence within the retained tail, before a clean user turn
// or complete tool cycle. Appending assistant text would create a provider
// prefill; appending user text would invent authority. Never enter the frozen
// prefix or split a call from its results. No safe slot means no transform.
func insertEvidenceNotice(messages, note *JSONValue, retained int) bool {
	if !messages.IsArray() || retained <= 0 {
		return false
	}
	minimum := messages.Len() - retained
	if minimum < 0 {
		minimum = 0
	}
	for i := messages.Len() - 1; i >= minimum; i-- {
		if isCleanUserTurn(messages.At(i)) || isAssistantToolTurn(messages.At(i)) {
			messages.Items = append(messages.Items, nil)
			copy(messages.Items[i+1:], messages.Items[i:len(messages.Items)-1])
			messages.Items[i] = note
			return true
		}
	}
	return false
}
