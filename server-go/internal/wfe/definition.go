package wfe

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"

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
	canonical, err := yaml.Marshal(def)
	if err != nil {
		return Definition{}, fmt.Errorf("canonicalize workflow definition: %w", err)
	}
	def.Version = Hash(canonical)
	return def, nil
}

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
			producer, output, ok := splitBinding(binding)
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

func splitBinding(binding string) (producer, output string, ok bool) {
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
