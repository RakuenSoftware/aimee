package memory

import (
	"encoding/json"
	"fmt"
	"strings"
)

// Apply the CLI's compact profile recursively and its top-level field filter,
// keeping number tokens intact instead of round-tripping IDs through float64.
func memoryJSONOutput(payload json.RawMessage, args commandArgs) (string, error) {
	if args.stringOr("profile", "") == "compact" {
		var err error
		payload, err = compactMemoryJSON(payload)
		if err != nil {
			return "", err
		}
	}
	if fields, exists := args.stringValue("fields"); exists {
		var err error
		payload, err = filterMemoryJSONFields(payload, fields)
		if err != nil {
			return "", err
		}
	}
	return string(payload), nil
}

// Match emit_json_ctx: filter an object or each object in the top-level array.
// Nested objects retain their fields; raw number tokens retain full int64 IDs.
func filterMemoryJSONFields(payload json.RawMessage, fields string) (json.RawMessage, error) {
	raw := strings.TrimSpace(string(payload))
	if strings.HasPrefix(raw, "[") {
		var rows []json.RawMessage
		if err := json.Unmarshal(payload, &rows); err != nil {
			return nil, err
		}
		for i, row := range rows {
			if strings.HasPrefix(strings.TrimSpace(string(row)), "{") {
				var err error
				rows[i], err = filterMemoryJSONFields(row, fields)
				if err != nil {
					return nil, err
				}
			}
		}
		return json.Marshal(rows)
	}
	if !strings.HasPrefix(raw, "{") {
		return payload, nil
	}
	var object map[string]json.RawMessage
	if err := json.Unmarshal(payload, &object); err != nil {
		return nil, err
	}
	filtered := map[string]json.RawMessage{}
	for _, field := range strings.Split(fields, ",") {
		if value, ok := object[strings.TrimSpace(field)]; ok {
			filtered[strings.TrimSpace(field)] = value
		}
	}
	if status, ok := object["status"]; ok {
		filtered["status"] = status
	}
	return json.Marshal(filtered)
}

func compactMemoryJSON(payload json.RawMessage) (json.RawMessage, error) {
	raw := strings.TrimSpace(string(payload))
	if strings.HasPrefix(raw, "{") {
		var object map[string]json.RawMessage
		if err := json.Unmarshal(payload, &object); err != nil {
			return nil, err
		}
		for _, field := range []string{"description", "sensitivity", "created_at", "updated_at", "scanned_at", "domain"} {
			delete(object, field)
		}
		for key, value := range object {
			var err error
			object[key], err = compactMemoryJSON(value)
			if err != nil {
				return nil, err
			}
		}
		return json.Marshal(object)
	}
	if strings.HasPrefix(raw, "[") {
		var values []json.RawMessage
		if err := json.Unmarshal(payload, &values); err != nil {
			return nil, err
		}
		for i, value := range values {
			var err error
			values[i], err = compactMemoryJSON(value)
			if err != nil {
				return nil, err
			}
		}
		return json.Marshal(values)
	}
	return payload, nil
}

func briefingText(b briefingBundle) string {
	var out strings.Builder
	fmt.Fprintf(&out, "# Session Briefing\n\n## Key Facts (%d)\n", len(b.Facts))
	for _, f := range b.Facts {
		fmt.Fprintf(&out, "  - [%s/%s #%d] %s\n", f.Tier, f.Kind, f.MemoryID, f.Text)
	}
	fmt.Fprintf(&out, "\n## Recent Activity (%d)\n", len(b.Activity))
	for _, a := range b.Activity {
		fmt.Fprintf(&out, "  - %s: %s\n", a.SessionID, a.Summary)
	}
	fmt.Fprintf(&out, "\n## Active Entities (%d)\n", len(b.Entities))
	for _, e := range b.Entities {
		fmt.Fprintf(&out, "  - %s (mentions=%d)\n", e.Name, e.Mentions)
	}
	fmt.Fprintf(&out, "\napprox_tokens=%d / limit_tokens=%d\n", b.ApproxTokens, b.LimitTokens)
	return out.String()
}

func memoryBundleOutput(verb string, payload json.RawMessage, args commandArgs, missing bool) (string, error) {
	switch args.stringOr("format", "") {
	case "text":
		if verb == "alerts" {
			var b alertsBundle
			if err := json.Unmarshal(payload, &b); err != nil {
				return "", err
			}
			return alertsText(b), nil
		}
		var b briefingBundle
		if err := json.Unmarshal(payload, &b); err != nil {
			return "", err
		}
		return briefingText(b), nil
	case "mcp":
		var object map[string]json.RawMessage
		if err := json.Unmarshal(payload, &object); err != nil {
			return "", err
		}
		object["active_context_missing"], _ = json.Marshal(missing)
		raw, err := json.Marshal(object)
		return string(raw), err
	default:
		return memoryJSONOutput(payload, args)
	}
}
