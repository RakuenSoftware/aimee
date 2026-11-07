package providers

import "errors"

// A connection owns dispatch and credentials together. Editing a model alias
// cannot select a different transport behind the connection owner's back.
func applyConnectionTransport(connection, input object) error {
	changed := false
	for _, field := range []string{"backend", "cli_kind", "cli_cmd", "session_reuse"} {
		if value, present := input[field]; present {
			if field == "session_reuse" {
				if _, ok := value.(bool); !ok {
					return errors.New("session_reuse must be a boolean")
				}
			} else if _, ok := value.(string); !ok {
				return errors.New(field + " must be a string")
			}
			connection[field] = value
			changed = true
		}
	}
	if !changed {
		return nil
	}
	switch str(connection, "backend") {
	case "", "http":
		for _, field := range []string{"cli_kind", "cli_cmd", "session_reuse"} {
			if value, present := input[field]; present && value != "" && value != false {
				return errors.New("native settings require provider-cli backend")
			}
			delete(connection, field)
		}
	case "provider-cli", "cli-stdio", "tmux-cli":
		connection["backend"] = "provider-cli"
		if str(connection, "cli_kind") == "" {
			return errors.New("native connection requires cli_kind")
		}
	default:
		return errors.New("backend must be http or provider-cli")
	}
	// Reuse the runtime's field validation and SDK defaults, copying only the
	// connection-owned fields back rather than adding model settings to a provider.
	normalized := copyObject(connection)
	if err := normalizeModel(normalized); err != nil {
		return err
	}
	for _, field := range []string{"backend", "cli_kind", "cli_cmd", "session_reuse"} {
		if value, present := normalized[field]; present {
			connection[field] = value
		}
	}
	return nil
}
