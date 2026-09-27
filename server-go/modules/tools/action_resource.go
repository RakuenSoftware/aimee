package tools

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	executionpolicy "github.com/JBailes/aimee/server-go/modules/execution-policy"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unicode/utf8"
)

// ActionResource is supplied by the tool owner, never a model's side_effect
// label. Unresolved external tools have no governed destination capability.
type ActionResource struct {
	Arguments     json.RawMessage `json:"effective_arguments,omitempty"`
	Class         string          `json:"class"`
	Destination   string          `json:"destination"`
	PayloadDigest string          `json:"payload_sha256"`
	RequestBytes  string          `json:"request_bytes"`
}

// Reject ambiguous JSON before comparing the Go resource decision with the C
// dispatcher's effective arguments. Duplicate keys or NULs cannot select two
// different paths at the two boundaries.
func actionJSONValue(d *json.Decoder, depth int) (any, error) {
	if depth > 64 {
		return nil, errors.New("action JSON nesting exceeded")
	}
	token, e := d.Token()
	if e != nil {
		return nil, e
	}
	switch v := token.(type) {
	case json.Delim:
		switch v {
		case '{':
			m := map[string]any{}
			for d.More() {
				key, e := d.Token()
				if e != nil {
					return nil, e
				}
				k, ok := key.(string)
				if !ok || strings.ContainsRune(k, 0) {
					return nil, errors.New("invalid action key")
				}
				if _, exists := m[k]; exists {
					return nil, errors.New("duplicate action key")
				}
				value, e := actionJSONValue(d, depth+1)
				if e != nil {
					return nil, e
				}
				m[k] = value
			}
			end, e := d.Token()
			if e != nil || end != json.Delim('}') {
				return nil, errors.New("invalid action object")
			}
			return m, nil
		case '[':
			a := []any{}
			for d.More() {
				value, e := actionJSONValue(d, depth+1)
				if e != nil {
					return nil, e
				}
				a = append(a, value)
			}
			end, e := d.Token()
			if e != nil || end != json.Delim(']') {
				return nil, errors.New("invalid action array")
			}
			return a, nil
		}
		return nil, errors.New("invalid action delimiter")
	case string:
		if strings.ContainsRune(v, 0) {
			return nil, errors.New("NUL in action argument")
		}
		return v, nil
	default:
		return token, nil
	}
}

func DescribeGovernedAction(tool string, raw []byte, cwd string) (ActionResource, error) {
	var out ActionResource
	if len(raw) == 0 || len(raw) > 32768 || !utf8.Valid(raw) || !filepath.IsAbs(cwd) {
		return out, errors.New("bounded effective arguments and host directory required")
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	value, err := actionJSONValue(decoder, 0)
	if err != nil {
		return out, err
	}
	if _, err = decoder.Token(); err != io.EOF {
		return out, errors.New("trailing action JSON")
	}
	args, ok := value.(map[string]any)
	if !ok {
		return out, errors.New("action object required")
	}
	switch tool {
	case "write_file", "edit_file", "edit_symbol":
		out.Class = "file_write"
	case "read_file", "read_symbol", "grep":
		out.Class = "read_only"
	default:
		return out, errors.New("tool has no exact governed resource adapter")
	}
	target, ok := args["path"].(string)
	if !ok || target == "" || len(target) > 4096 {
		return out, errors.New("exact path required")
	}
	if !filepath.IsAbs(target) {
		target = filepath.Join(cwd, target)
	}
	target = filepath.Clean(target)
	// Resolve directory aliases. A missing final file is valid for write_file;
	// an unavailable parent is a reconstruction gap, not permission to guess.
	parent, err := filepath.EvalSymlinks(filepath.Dir(target))
	if err != nil {
		return out, errors.New("target directory unavailable")
	}
	target = filepath.Join(parent, filepath.Base(target))
	if resolved, e := filepath.EvalSymlinks(target); e == nil {
		target = resolved
	} else if _, statErr := os.Lstat(target); !errors.Is(statErr, os.ErrNotExist) {
		return out, errors.New("existing target cannot be resolved exactly")
	}
	out.Class, err = executionpolicy.FileActionClass(out.Class == "file_write", target)
	if err != nil {
		return out, err
	}
	args["path"] = target
	canonical, err := json.Marshal(args)
	if err != nil {
		return out, err
	}
	if len(canonical) > 32768 {
		return out, errors.New("canonical action exceeds byte limit")
	}
	out.Arguments = canonical
	digest := sha256.Sum256(canonical)
	out.Destination = "file:" + target
	out.PayloadDigest = hex.EncodeToString(digest[:])
	out.RequestBytes = strconv.Itoa(len(canonical))
	return out, nil
}
