package wfe

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"

	"go.yaml.in/yaml/v3"
)

var nodeIDPattern = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_-]*$`)

type Definition struct {
	Name       string   `yaml:"name" json:"name"`
	IntentTags []string `yaml:"intent_tags,omitempty" json:"intent_tags,omitempty"`
	Start      string   `yaml:"start" json:"start"`
	Enforced   bool     `yaml:"enforced,omitempty" json:"enforced"`
	Nodes      []Node   `yaml:"nodes" json:"nodes"`
	Version    string   `yaml:"-" json:"version"`
}

type Node struct {
	ID     string            `yaml:"id" json:"id"`
	Block  string            `yaml:"block" json:"block"`
	In     map[string]string `yaml:"in,omitempty" json:"in,omitempty"`
	Params map[string]any    `yaml:"params,omitempty" json:"params,omitempty"`
	Next   string            `yaml:"next,omitempty" json:"next,omitempty"`
	OnPass string            `yaml:"on_pass,omitempty" json:"on_pass,omitempty"`
	OnFail string            `yaml:"on_fail,omitempty" json:"on_fail,omitempty"`
}

func ParseDefinition(content []byte) (Definition, error) {
	decoder := yaml.NewDecoder(bytes.NewReader(content))
	decoder.KnownFields(true)
	var def Definition
	if err := decoder.Decode(&def); err != nil {
		return Definition{}, fmt.Errorf("decode workflow definition: %w", err)
	}
	var extra any
	if err := decoder.Decode(&extra); err == nil {
		return Definition{}, errors.New("workflow definition must contain one YAML document")
	} else if !errors.Is(err, io.EOF) {
		return Definition{}, fmt.Errorf("decode trailing workflow content: %w", err)
	}
	if err := def.Validate(); err != nil {
		return Definition{}, err
	}
	canonical, err := CanonicalDefinition(def)
	if err != nil {
		return Definition{}, fmt.Errorf("canonicalize workflow definition: %w", err)
	}
	def.Version = Hash(canonical)
	return def, nil
}

func canonicalYAML(def Definition) ([]byte, error) { return yaml.Marshal(def) }

func LoadDefinition(path string) (Definition, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return Definition{}, fmt.Errorf("read workflow definition: %w", err)
	}
	return ParseDefinition(content)
}

func (d Definition) Validate() error {
	if d.Name == "" {
		return errors.New("workflow name is required")
	}
	if len(d.Nodes) == 0 {
		return errors.New("workflow must contain at least one node")
	}
	start := d.Start
	if start == "" {
		start = d.Nodes[0].ID
	}
	nodes := make(map[string]Node, len(d.Nodes))
	for _, node := range d.Nodes {
		if !nodeIDPattern.MatchString(node.ID) {
			return fmt.Errorf("invalid node id %q", node.ID)
		}
		if node.Block == "" {
			return fmt.Errorf("node %q has no block", node.ID)
		}
		if _, exists := nodes[node.ID]; exists {
			return fmt.Errorf("duplicate node id %q", node.ID)
		}
		nodes[node.ID] = node
	}
	if _, ok := nodes[start]; !ok {
		return fmt.Errorf("start node %q does not exist", start)
	}
	for _, node := range d.Nodes {
		for edgeName, target := range map[string]string{
			"next": node.Next, "on_pass": node.OnPass, "on_fail": node.OnFail,
		} {
			if target == "" {
				continue
			}
			if _, ok := nodes[target]; !ok {
				return fmt.Errorf("node %q %s target %q does not exist", node.ID, edgeName, target)
			}
		}
		for input, binding := range node.In {
			producer, output, ok := ParseBinding(binding)
			if !ok {
				return fmt.Errorf("node %q input %q has invalid binding %q", node.ID, input, binding)
			}
			if _, exists := nodes[producer]; !exists {
				return fmt.Errorf("node %q input %q references missing producer %q", node.ID, input, producer)
			}
			if output != "out" {
				return fmt.Errorf("node %q input %q references unsupported output %q", node.ID, input, output)
			}
		}
		// Built-in required params are checked here, not only in ValidateCatalog,
		// so no load path can reach the runner with a block that has nothing to
		// resolve. Custom blocks declare theirs and are covered by ValidateCatalog.
		if block, builtIn := BuiltinCatalog()[node.Block]; builtIn {
			if err := requiredParams(node, block); err != nil {
				return err
			}
		}
		if node.Block == "trigger.watch-dir" {
			if err := validateWatchDirParams(node); err != nil {
				return err
			}
		}
		if node.Block == "gate.roundtable" {
			panel, _ := node.Params["panel"].(map[string]any)
			if panel != nil {
				if _, legacy := panel["personas"]; legacy {
					return fmt.Errorf("node %q panel.personas is unsupported; use panel.required/eligible and panel.pins", node.ID)
				}
				personas := map[string]bool{}
				for _, field := range []string{"required", "eligible"} {
					values, ok := panel[field].([]any)
					if !ok {
						continue
					}
					for _, raw := range values {
						text, ok := raw.(string)
						if !ok || text == "" {
							return fmt.Errorf("node %q panel.%s must contain persona names", node.ID, field)
						}
						if personas[text] {
							return fmt.Errorf("node %q panel persona %q is duplicated", node.ID, text)
						}
						personas[text] = true
					}
				}
				if pins, ok := panel["pins"].(map[string]any); ok {
					for persona, raw := range pins {
						agent, ok := raw.(string)
						if !ok || agent == "" || !personas[persona] {
							return fmt.Errorf("node %q panel pin %q must name a configured persona and specific agent", node.ID, persona)
						}
					}
				}
				if len(personas) == 0 {
					return fmt.Errorf("node %q panel requires at least one persona", node.ID)
				}
				if raw, ok := node.Params["quorum"]; ok {
					q, ok := numericInt(raw)
					if !ok || q < 1 || q > len(personas) {
						return fmt.Errorf("node %q quorum must be between 1 and %d", node.ID, len(personas))
					}
				}
			}
		}
	}
	return nil
}

func validateWatchDirParams(node Node) error {
	stringParam := func(key string) (string, bool, error) {
		raw, present := node.Params[key]
		if !present {
			return "", false, nil
		}
		value, ok := raw.(string)
		if !ok {
			return "", true, fmt.Errorf("node %q param %q must be a string", node.ID, key)
		}
		return value, true, nil
	}
	if workspace, present, err := stringParam("workspace"); err != nil {
		return err
	} else if present && workspace != "" && (workspace != strings.TrimSpace(workspace) || !filepath.IsAbs(workspace) ||
		strings.IndexFunc(workspace, func(r rune) bool { return r < 0x20 || r == 0x7f }) >= 0) {
		return fmt.Errorf("node %q param %q must be an absolute server path with no surrounding whitespace or control characters", node.ID, "workspace")
	}
	if directory, present, err := stringParam("dir"); err != nil {
		return err
	} else if present && directory != "" {
		trimmed := strings.TrimSpace(directory)
		clean := path.Clean(trimmed)
		if directory != trimmed || strings.Contains(directory, "\\") ||
			strings.IndexFunc(directory, func(r rune) bool { return r < 0x20 || r == 0x7f }) >= 0 ||
			clean == "." || path.IsAbs(clean) || clean == ".." || strings.HasPrefix(clean, "../") {
			return fmt.Errorf("node %q param %q must be a confined repository-relative directory", node.ID, "dir")
		}
	}
	if ref, present, err := stringParam("ref"); err != nil {
		return err
	} else if present && (ref != strings.TrimSpace(ref) || strings.HasPrefix(ref, "-") ||
		strings.IndexFunc(ref, func(r rune) bool { return r < 0x20 || r == 0x7f }) >= 0) {
		return fmt.Errorf("node %q param %q must not contain surrounding whitespace, control characters, or start with '-'", node.ID, "ref")
	}
	if mode, present, err := stringParam("mode"); err != nil {
		return err
	} else if present && mode != "" && mode != "autonomous" && mode != "interactive" {
		return fmt.Errorf("node %q param %q must be autonomous or interactive", node.ID, "mode")
	}
	if raw, present := node.Params["max_spend_usd"]; present {
		spend, ok := numericFloat(raw)
		if !ok || spend < 0 || math.IsNaN(spend) || math.IsInf(spend, 0) {
			return fmt.Errorf("node %q param %q must be finite and non-negative", node.ID, "max_spend_usd")
		}
	}
	return nil
}

func (d Definition) Node(id string) (Node, bool) {
	for _, node := range d.Nodes {
		if node.ID == id {
			return node, true
		}
	}
	return Node{}, false
}

func ParseBinding(binding string) (producer, output string, ok bool) {
	for i := len(binding) - 1; i >= 0; i-- {
		if binding[i] == '.' {
			if i == 0 || i == len(binding)-1 {
				return "", "", false
			}
			return binding[:i], binding[i+1:], true
		}
	}
	return "", "", false
}

func numericInt(value any) (int, bool) {
	switch n := value.(type) {
	case int:
		return n, true
	case int64:
		return int(n), true
	case uint64:
		return int(n), true
	case float64:
		if n == float64(int(n)) {
			return int(n), true
		}
	}
	return 0, false
}

func numericFloat(value any) (float64, bool) {
	switch n := value.(type) {
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case uint64:
		return float64(n), true
	case float64:
		return n, true
	}
	return 0, false
}
