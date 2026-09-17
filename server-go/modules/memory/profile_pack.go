package memory

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

type profilePack struct {
	Name              string   `json:"name"`
	Description       string   `json:"description"`
	AllowedTiers      []string `json:"allowed_tiers,omitempty"`
	AllowedKinds      []string `json:"allowed_kinds,omitempty"`
	DefaultTier       string   `json:"default_tier,omitempty"`
	DefaultVisibility string   `json:"default_visibility,omitempty"`
}

func profilePackDir() (string, error) {
	if dir := os.Getenv("AIMEE_PACK_DIR"); dir != "" {
		return dir, nil
	}
	if home := os.Getenv("AIMEE_HOME"); home != "" {
		return filepath.Join(home, "packs"), nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	base := filepath.Join(home, ".config", "aimee")
	if profile := os.Getenv("AIMEE_PROFILE"); profile != "" {
		base = filepath.Join(base, "profiles", profile)
	}
	return filepath.Join(base, "packs"), nil
}

func validPackName(name string) bool {
	return name != "" && name != "." && name != ".." && len(name) < 64 && !strings.ContainsAny(name, "/\\\x00\r\n")
}

func readProfilePack(path string) (profilePack, error) {
	var pack profilePack
	file, err := os.Open(path)
	if err != nil {
		return pack, err
	}
	defer file.Close()
	body, err := io.ReadAll(io.LimitReader(file, 65537))
	if err != nil {
		return pack, err
	}
	if len(body) == 0 || len(body) > 65536 {
		return pack, errors.New("file size out of range")
	}
	var root commandArgs
	if json.Unmarshal(body, &root) != nil || root == nil {
		return pack, errors.New("invalid JSON object")
	}
	var ok bool
	pack.Name, ok = root.stringValue("name")
	if !ok || pack.Name == "" {
		return pack, errors.New("missing required field: name")
	}
	pack.Description, ok = root.stringValue("description")
	if !ok {
		return pack, errors.New("missing required field: description")
	}
	for name, raw := range root {
		switch name {
		case "name", "description":
		case "validation_rules", "scope_defaults", "extraction_hints", "explain_labels":
			var object commandArgs
			if json.Unmarshal(raw, &object) != nil || object == nil {
				return pack, fmt.Errorf("%s must be an object", name)
			}
			if name == "validation_rules" {
				for _, field := range []string{"allowed_tiers", "allowed_kinds"} {
					if raw, exists := object[field]; exists {
						var items []json.RawMessage
						if string(raw) == "null" || json.Unmarshal(raw, &items) != nil {
							return pack, fmt.Errorf("validation_rules.%s must be an array", field)
						}
						for _, raw := range items {
							var value string
							isString := string(raw) != "null" && json.Unmarshal(raw, &value) == nil
							if field == "allowed_tiers" {
								if !isString || (value != "L0" && value != "L1" && value != "L2" && value != "L3") {
									return pack, fmt.Errorf("unknown tier: %s", raw)
								}
								if len(pack.AllowedTiers) < 8 {
									pack.AllowedTiers = append(pack.AllowedTiers, value)
								}
							} else if isString && len(pack.AllowedKinds) < 16 {
								pack.AllowedKinds = append(pack.AllowedKinds, value)
							}
						}
					}
				}
			} else if name == "scope_defaults" {
				pack.DefaultTier, pack.DefaultVisibility = object.stringOr("tier", ""), object.stringOr("visibility", "")
			}
		default:
			return pack, fmt.Errorf("unknown key: %s", name)
		}
	}
	return pack, nil
}

func loadProfilePack(dir, name string) (profilePack, error) {
	if !validPackName(name) {
		return profilePack{}, errors.New("invalid pack name")
	}
	return readProfilePack(filepath.Join(dir, name+".json"))
}

func activeProfilePack(dir string) string {
	file, err := os.Open(filepath.Join(dir, "active-pack"))
	if err != nil {
		return "default"
	}
	defer file.Close()
	body, err := io.ReadAll(io.LimitReader(file, 65))
	name := strings.TrimRight(string(body), "\r\n")
	if err != nil || !validPackName(name) {
		return "default"
	}
	return name
}

func activateProfilePack(dir, name string) error {
	if _, err := loadProfilePack(dir, name); err != nil {
		return err
	}
	file, err := os.CreateTemp(dir, ".active-pack-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err = file.WriteString(name + "\n"); err == nil {
		err = file.Sync()
	}
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(file.Name(), filepath.Join(dir, "active-pack"))
}

func handlePackCommand(_ handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	dir, err := profilePackDir()
	if err != nil {
		return commandResult(commandError("unavailable", err.Error()))
	}
	var output any
	var text strings.Builder
	switch args.stringOr("action", "") {
	case "list":
		entries, readErr := os.ReadDir(dir)
		if readErr != nil && !os.IsNotExist(readErr) {
			err = readErr
			break
		}
		active := activeProfilePack(dir)
		packs := []map[string]any{}
		for _, entry := range entries {
			name := strings.TrimSuffix(entry.Name(), ".json")
			if entry.IsDir() || name == entry.Name() || !validPackName(name) {
				continue
			}
			packs = append(packs, map[string]any{"name": name, "active": name == active})
			text.WriteString(name)
			if name == active {
				text.WriteString(" (active)")
			}
			text.WriteByte('\n')
			if len(packs) == 64 {
				break
			}
		}
		if len(packs) == 0 {
			fmt.Fprintf(&text, "No profile packs found in %s\n", dir)
		}
		output = map[string]any{"packs": packs, "active": active}
	case "show":
		var pack profilePack
		pack, err = loadProfilePack(dir, args.stringOr("name", ""))
		if err != nil {
			break
		}
		output = pack
		fmt.Fprintf(&text, "name:        %s\ndescription: %s\n", pack.Name, pack.Description)
		if len(pack.AllowedTiers) > 0 {
			fmt.Fprintf(&text, "allowed_tiers: %s\n", strings.Join(pack.AllowedTiers, " "))
		}
		if len(pack.AllowedKinds) > 0 {
			fmt.Fprintf(&text, "allowed_kinds: %s\n", strings.Join(pack.AllowedKinds, " "))
		}
		if pack.DefaultTier != "" {
			fmt.Fprintf(&text, "default_tier: %s\n", pack.DefaultTier)
		}
		if pack.DefaultVisibility != "" {
			fmt.Fprintf(&text, "default_visibility: %s\n", pack.DefaultVisibility)
		}
	case "validate":
		path := args.stringOr("path", "")
		_, err = readProfilePack(path)
		output = map[string]any{"status": "ok"}
		fmt.Fprintf(&text, "ok: %s\n", path)
	case "use":
		name := args.stringOr("name", "")
		err = activateProfilePack(dir, name)
		output = map[string]any{"status": "ok"}
		fmt.Fprintf(&text, "active pack set to: %s\n", name)
	default:
		return commandResult(commandError("invalid_argument", "memory pack requires list, show, validate or use"))
	}
	if err != nil {
		return commandResult(commandError("invalid_argument", err.Error()))
	}
	return commandResult(map[string]any{"status": "ok", "output": output, "text": text.String()})
}
