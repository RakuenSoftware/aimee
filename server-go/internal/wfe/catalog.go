package wfe

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"go.yaml.in/yaml/v3"
)

type BlockDefinition struct {
	Name             string   `json:"name" yaml:"name"`
	Produces         string   `json:"produces" yaml:"produces,omitempty"`
	Accepts          []string `json:"accepts" yaml:"-"`
	InputPorts       []string `json:"input_ports" yaml:"-"`
	RequiredPorts    []string `json:"required_ports" yaml:"-"`
	Custom           bool     `json:"custom" yaml:"-"`
	RequiresInput    bool     `json:"requires_input" yaml:"-"`
	Executor         string   `json:"executor,omitempty" yaml:"executor,omitempty"`
	Consumes         string   `json:"consumes,omitempty" yaml:"consumes,omitempty"`
	Persona          string   `json:"persona,omitempty" yaml:"persona,omitempty"`
	Prompt           string   `json:"prompt,omitempty" yaml:"prompt,omitempty"`
	Command          []string `json:"command,omitempty" yaml:"command,omitempty"`
	CommandSHA256    string   `json:"command_sha256,omitempty" yaml:"command_sha256,omitempty"`
	CommandTimeoutMS int      `json:"command_timeout_ms,omitempty" yaml:"-"`
}

type customBlockFile struct {
	AllowCommand     bool              `yaml:"allow_command"`
	CommandTimeoutMS int               `yaml:"command_timeout_ms"`
	Blocks           []BlockDefinition `yaml:"blocks"`
}

var BuiltinBlocks = []BlockDefinition{
	{Name: "author.proposal", Produces: "proposal", Accepts: []string{"proposal"}, InputPorts: []string{"proposal"}},
	{Name: "trigger.watch-dir", Produces: "proposal"},
	{Name: "author.plan", Produces: "plan", Accepts: []string{"proposal"}, InputPorts: []string{"proposal"}, RequiredPorts: []string{"proposal"}, RequiresInput: true},
	{Name: "implement", Produces: "branch", Accepts: []string{"plan", "intent"}, InputPorts: []string{"plan"}, RequiredPorts: []string{"plan"}, RequiresInput: true},
	{Name: "document", Produces: "branch", Accepts: []string{"branch"}, InputPorts: []string{"branch"}, RequiredPorts: []string{"branch"}, RequiresInput: true},
	{Name: "source.archive", Produces: "branch", Accepts: []string{"branch"}, InputPorts: []string{"branch"}, RequiredPorts: []string{"branch"}, RequiresInput: true},
	{Name: "freeze", Produces: "frozen_diff", Accepts: []string{"branch"}, InputPorts: []string{"branch"}, RequiredPorts: []string{"branch"}, RequiresInput: true},
	{Name: "gate.roundtable", Produces: "verdict", Accepts: []string{"proposal", "plan", "frozen_diff"}, InputPorts: []string{"src"}, RequiredPorts: []string{"src"}, RequiresInput: true},
	{Name: "gate.human", Produces: "approval", Accepts: []string{"proposal", "plan", "branch", "frozen_diff", "pr"}, InputPorts: []string{"src"}, RequiredPorts: []string{"src"}, RequiresInput: true},
	{Name: "pr.open", Produces: "pr", Accepts: []string{"proposal", "frozen_diff"}, InputPorts: []string{"src"}, RequiredPorts: []string{"src"}, RequiresInput: true},
	{Name: "merge", Produces: "none", Accepts: []string{"pr"}, InputPorts: []string{"pr"}, RequiredPorts: []string{"pr"}, RequiresInput: true},
	{Name: "gate.ci", Produces: "verdict", Accepts: []string{"pr"}, InputPorts: []string{"pr"}, RequiredPorts: []string{"pr"}, RequiresInput: true},
	{Name: "check.mergeable", Produces: "verdict", Accepts: []string{"pr"}, InputPorts: []string{"pr"}, RequiredPorts: []string{"pr"}, RequiresInput: true},
	{Name: "understand", Produces: "intent"},
	{Name: "split", Produces: "plan", Accepts: []string{"intent", "plan"}, InputPorts: []string{"intent", "plan"}, RequiresInput: true},
	{Name: "review", Produces: "verdict", Accepts: []string{"frozen_diff", "branch"}, InputPorts: []string{"src"}, RequiredPorts: []string{"src"}, RequiresInput: true},
	{Name: "gate.deliver", Produces: "none", Accepts: []string{"verdict", "approval"}, InputPorts: []string{"verdict"}, RequiredPorts: []string{"verdict"}, RequiresInput: true},
	{Name: "branch.open", Produces: "branch", Accepts: []string{"plan"}},
	{Name: "foreach.workflow", Produces: "branch", Accepts: []string{"plan", "branch"}, InputPorts: []string{"packets", "feature"}, RequiredPorts: []string{"packets", "feature"}, RequiresInput: true},
}

func BuiltinCatalog() map[string]BlockDefinition {
	out := make(map[string]BlockDefinition, len(BuiltinBlocks))
	for _, block := range BuiltinBlocks {
		out[block.Name] = block
	}
	return out
}

func (r *Registry) Catalog() (map[string]BlockDefinition, error) {
	catalog := BuiltinCatalog()
	content, err := os.ReadFile(filepath.Join(r.dir, "blocks.yaml"))
	if errors.Is(err, os.ErrNotExist) {
		return catalog, nil
	}
	if err != nil {
		return nil, err
	}
	var file customBlockFile
	if err := yaml.Unmarshal(content, &file); err != nil {
		return nil, fmt.Errorf("parse custom blocks: %w", err)
	}
	for _, block := range file.Blocks {
		if !SafeName(block.Name) {
			return nil, fmt.Errorf("invalid custom block name %q", block.Name)
		}
		if _, exists := catalog[block.Name]; exists {
			return nil, fmt.Errorf("custom block %q shadows a built-in", block.Name)
		}
		if block.Consumes == "" {
			block.Consumes = "none"
		}
		if block.Produces == "" {
			block.Produces = "none"
		}
		block.Custom = true
		block.InputPorts = []string{"in"}
		if block.RequiresInput {
			block.RequiredPorts = []string{"in"}
		}
		block.RequiresInput = block.Consumes != "none"
		if block.RequiresInput {
			block.Accepts = []string{block.Consumes}
		} else {
			block.Accepts = []string{}
		}
		if block.Executor == "" {
			block.Executor = "delegate"
		}
		if block.Executor == "delegate" && (block.Persona == "" || block.Prompt == "") {
			return nil, fmt.Errorf("delegate block %q requires persona and prompt", block.Name)
		}
		if block.Executor == "command" {
			if !file.AllowCommand {
				return nil, fmt.Errorf("command block %q is disabled", block.Name)
			}
			if len(block.Command) == 0 {
				return nil, fmt.Errorf("command block %q has no argv", block.Name)
			}
			if !filepath.IsAbs(block.Command[0]) {
				return nil, fmt.Errorf("command block %q executable must be absolute", block.Name)
			}
			binary, err := os.ReadFile(block.Command[0])
			if err != nil {
				return nil, fmt.Errorf("read command block %q executable: %w", block.Name, err)
			}
			digest := sha256.Sum256(binary)
			if decoded, err := hex.DecodeString(block.CommandSHA256); err != nil || len(decoded) != sha256.Size || block.CommandSHA256 != hex.EncodeToString(digest[:]) {
				return nil, fmt.Errorf("command block %q executable sha256 mismatch", block.Name)
			}
			block.CommandTimeoutMS = file.CommandTimeoutMS
		}
		catalog[block.Name] = block
	}
	return catalog, nil
}

func (r *Registry) Blocks() ([]BlockDefinition, error) {
	catalog, err := r.Catalog()
	if err != nil {
		return nil, err
	}
	out := make([]BlockDefinition, 0, len(catalog))
	for _, block := range catalog {
		if block.Accepts == nil {
			block.Accepts = []string{}
		}
		out = append(out, block)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

func (r *Registry) SaveDelegateBlock(block BlockDefinition) error {
	unlock, err := r.lockWrites()
	if err != nil {
		return err
	}
	defer unlock()
	if !SafeName(block.Name) {
		return errors.New("invalid block name")
	}
	if _, builtIn := BuiltinCatalog()[block.Name]; builtIn {
		return errors.New("name shadows a built-in block")
	}
	if block.Persona == "" || block.Prompt == "" {
		return errors.New("delegate block needs a persona and prompt")
	}
	if block.Consumes == "" {
		block.Consumes = "none"
	}
	if block.Produces != "branch" && block.Produces != "none" {
		return errors.New("produces must be branch or none")
	}
	file, err := r.readCustomFile()
	if err != nil {
		return err
	}
	block.Executor = "delegate"
	block.Custom = false
	replaced := false
	for i, existing := range file.Blocks {
		if existing.Name == block.Name {
			if existing.Executor == "command" {
				return errors.New("command blocks are operator-managed")
			}
			file.Blocks[i] = block
			replaced = true
		}
	}
	if !replaced {
		file.Blocks = append(file.Blocks, block)
	}
	return r.writeCustomFile(file)
}

func (r *Registry) DeleteDelegateBlock(name string) error {
	unlock, lockErr := r.lockWrites()
	if lockErr != nil {
		return lockErr
	}
	defer unlock()
	file, err := r.readCustomFile()
	if err != nil {
		return err
	}
	found := false
	out := file.Blocks[:0]
	for _, block := range file.Blocks {
		if block.Name == name {
			if block.Executor == "command" {
				return errors.New("command blocks are operator-managed")
			}
			found = true
			continue
		}
		out = append(out, block)
	}
	if !found {
		return os.ErrNotExist
	}
	file.Blocks = out
	return r.writeCustomFile(file)
}

func (r *Registry) readCustomFile() (customBlockFile, error) {
	var file customBlockFile
	content, err := os.ReadFile(filepath.Join(r.dir, "blocks.yaml"))
	if errors.Is(err, os.ErrNotExist) {
		return file, nil
	}
	if err != nil {
		return file, err
	}
	err = yaml.Unmarshal(content, &file)
	return file, err
}
func (r *Registry) writeCustomFile(file customBlockFile) error {
	content, err := yaml.Marshal(file)
	if err != nil {
		return err
	}
	path := filepath.Join(r.dir, "blocks.yaml")
	tmp, err := os.CreateTemp(r.dir, ".blocks.*.tmp")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if err := tmp.Chmod(0o600); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := writeAndSync(tmp, content); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(name, path)
}

func (d Definition) ValidateCatalog(catalog map[string]BlockDefinition) error {
	if err := d.Validate(); err != nil {
		return err
	}
	produces := make(map[string]string, len(d.Nodes))
	for _, node := range d.Nodes {
		block, ok := catalog[node.Block]
		if !ok {
			return fmt.Errorf("node %q uses unknown block %q", node.ID, node.Block)
		}
		produces[node.ID] = block.Produces
		if block.RequiresInput && len(node.In) == 0 {
			return fmt.Errorf("node %q requires an input", node.ID)
		}
		for _, required := range block.RequiredPorts {
			if _, ok := node.In[required]; !ok {
				return fmt.Errorf("node %q requires input port %q", node.ID, required)
			}
		}
	}
	for _, node := range d.Nodes {
		block := catalog[node.Block]
		for inputName, binding := range node.In {
			if !containsString(block.InputPorts, inputName) {
				return fmt.Errorf("node %q has unknown input port %q for block %q", node.ID, inputName, node.Block)
			}
			producer, _, _ := ParseBinding(binding)
			produced := produces[producer]
			accepted := false
			for _, kind := range block.Accepts {
				if kind == produced {
					accepted = true
					break
				}
			}
			if !accepted {
				return fmt.Errorf("node %q input %q cannot accept %s from %q", node.ID, inputName, produced, producer)
			}
		}
	}
	return nil
}

func containsString(values []string, value string) bool {
	for _, candidate := range values {
		if candidate == value {
			return true
		}
	}
	return false
}
