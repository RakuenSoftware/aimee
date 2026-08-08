package config

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"math"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"

	"go.yaml.in/yaml/v3"
)

type Store struct {
	path string
	mu   sync.Mutex
}

// MaxTriggerRules keeps the human-editable Go registry aligned with the
// long-standing C config schema. More rules than this are almost certainly an
// accidental duplicate/import and would make every scheduler scan expensive.
const MaxTriggerRules = 32

// TriggerRule is the persisted trigger_rules shape exposed by the Workflows UI.
// Pipeline remains nested in YAML; callers never need to reinterpret flattened
// config keys or maintain a second trigger configuration.
type TriggerRule struct {
	Source   string          `yaml:"source" json:"source"`
	Event    string          `yaml:"event,omitempty" json:"event"`
	Schedule string          `yaml:"schedule,omitempty" json:"schedule"`
	Mode     string          `yaml:"mode,omitempty" json:"mode"`
	Pipeline TriggerPipeline `yaml:"pipeline" json:"pipeline"`
}

type TriggerPipeline struct {
	Template    string  `yaml:"template" json:"template"`
	Workspace   string  `yaml:"workspace" json:"workspace"`
	MaxSpendUSD float64 `yaml:"max_spend_usd,omitempty" json:"max_spend_usd,omitempty"`
}

func (s *Store) TriggerRules() ([]TriggerRule, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	root, err := s.read()
	if err != nil {
		return nil, err
	}
	node := mappingChild(root, "trigger_rules", false)
	if node == nil {
		return []TriggerRule{}, nil
	}
	if node.Kind != yaml.SequenceNode {
		return nil, errors.New("trigger_rules must be a sequence")
	}
	var rules []TriggerRule
	if err := node.Decode(&rules); err != nil {
		return nil, fmt.Errorf("decode trigger_rules: %w", err)
	}
	if len(rules) > MaxTriggerRules {
		return nil, fmt.Errorf("trigger_rules has %d rules; maximum is %d", len(rules), MaxTriggerRules)
	}
	for i := range rules {
		if rules[i].Source == "" {
			return nil, fmt.Errorf("trigger_rules[%d].source is required", i)
		}
		if rules[i].Mode == "" {
			rules[i].Mode = "autonomous"
		}
		if rules[i].Mode != "autonomous" && rules[i].Mode != "interactive" {
			return nil, fmt.Errorf("trigger_rules[%d].mode must be autonomous or interactive", i)
		}
		if rules[i].Source == "watch-dir" || rules[i].Source == "proposals" {
			if rules[i].Event == "" {
				rules[i].Event = "docs/proposals/pending"
			}
			if !confinedTriggerDirectory(rules[i].Event) {
				return nil, fmt.Errorf("trigger_rules[%d].event must be a repository-relative directory", i)
			}
			if strings.HasPrefix(strings.TrimSpace(rules[i].Schedule), "-") {
				return nil, fmt.Errorf("trigger_rules[%d].schedule cannot start with '-'", i)
			}
			if rules[i].Pipeline.Template == "" || rules[i].Pipeline.Workspace == "" {
				return nil, fmt.Errorf("trigger_rules[%d] needs pipeline.template and pipeline.workspace", i)
			}
			if !validTriggerWorkspace(rules[i].Pipeline.Workspace) {
				return nil, fmt.Errorf("trigger_rules[%d].pipeline.workspace must be an absolute server path", i)
			}
			if rules[i].Pipeline.MaxSpendUSD < 0 || math.IsNaN(rules[i].Pipeline.MaxSpendUSD) || math.IsInf(rules[i].Pipeline.MaxSpendUSD, 0) {
				return nil, fmt.Errorf("trigger_rules[%d].pipeline.max_spend_usd must be finite and non-negative", i)
			}
		}
	}
	return rules, nil
}

func NewStore(path string) (*Store, error) {
	if path == "" {
		return nil, errors.New("config path is required")
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	return &Store{path: abs}, nil
}

var policyDefaults = map[string]any{
	"trigger.max_concurrent":            2,
	"trigger.scan_interval_secs":        5,
	"autonomy.auto_resume_cap_parks":    true,
	"autonomy.max_wall_secs":            1800,
	"autonomy.max_turns":                300,
	"autonomy.max_resumes":              50,
	"autonomy.stale_abandon_secs":       3600,
	"autonomy.concurrency":              5,
	"autonomy.per_workflow_concurrency": 1,
	"autonomy.delegate_pending_secs":    120,
}

var configurableTypes = map[string]string{
	"trigger.max_concurrent": "int", "autonomy.auto_resume_cap_parks": "bool",
	"trigger.scan_interval_secs": "int",
	"autonomy.max_wall_secs":     "int", "autonomy.max_turns": "int",
	"autonomy.max_resumes": "int", "autonomy.stale_abandon_secs": "int",
	"autonomy.concurrency":              "int",
	"autonomy.per_workflow_concurrency": "int",
	"autonomy.delegate_pending_secs":    "int",
}

type intBounds struct{ min, max int64 }

var configurableIntBounds = map[string]intBounds{
	"autonomy.delegate_pending_secs": {min: 2, max: 3600},
}

// The write-role wall floor. A write dispatch reserves time for the mandatory
// repository verifier and refuses outright below one viable model-call window, so
// under this many seconds every implement attempt refuses before it starts: the
// stage can never finish, no matter how often it retries. Rejecting the value
// when it is set says so up front instead of leaving it to be inferred from
// attempts dying.
//
// These two components are the engine's ALREADY-SHIPPED delegateWriteVerifyReserve
// and delegateWriteMinRunBudget, spelled out so the floor reads as what it is:
// the arithmetic consequence of existing behaviour, not a new policy number. The
// proposal that asked for this check excludes choosing how long a delegate may
// run, and this does not choose that — it rejects only the caps under which the
// shipped code already guarantees no attempt can finish.
//
// Duplicated rather than imported because internal/engine already imports this
// package; TestWriteRoleWallFloorMatchesConfigBound there fails if they drift.
const (
	writeVerifyReserveSecs = 300 // engine delegateWriteVerifyReserve (5m)
	writeMinRunSecs        = 60  // engine delegateWriteMinRunBudget (1m)

	MinAutonomyMaxWallSecs = writeVerifyReserveSecs + writeMinRunSecs
)

func (s *Store) Values() (map[string]any, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	root, err := s.read()
	if err != nil {
		return nil, err
	}
	out := make(map[string]any)
	flatten(root, "", out)
	for key := range out {
		if key != "trigger_rules" {
			if _, allowed := configurableTypes[key]; !allowed {
				delete(out, key)
			}
		}
	}
	for key, value := range policyDefaults {
		if _, ok := out[key]; !ok {
			out[key] = value
		}
	}
	return out, nil
}

func (s *Store) Value(key string) (any, error) {
	values, err := s.Values()
	if err != nil {
		return nil, err
	}
	value, ok := values[key]
	if !ok {
		return nil, fmt.Errorf("unknown config key %q", key)
	}
	return value, nil
}

func (s *Store) Int(key string, fallback int) int {
	value, ok, err := s.IntValue(key)
	if err != nil || !ok {
		return fallback
	}
	return value
}

func (s *Store) IntValue(key string) (int, bool, error) {
	value, err := s.Value(key)
	if err != nil {
		if strings.Contains(err.Error(), "unknown config key") {
			return 0, false, nil
		}
		return 0, false, err
	}
	switch n := value.(type) {
	case int:
		return n, true, nil
	case int64:
		return int(n), true, nil
	case uint64:
		return int(n), true, nil
	case float64:
		if n == float64(int(n)) {
			return int(n), true, nil
		}
	}
	return 0, false, fmt.Errorf("config key %q is not an integer", key)
}

func (s *Store) Bool(key string, fallback bool) bool {
	value, ok, err := s.BoolValue(key)
	if err != nil || !ok {
		return fallback
	}
	return value
}

func (s *Store) BoolValue(key string) (bool, bool, error) {
	value, err := s.Value(key)
	if err != nil {
		if strings.Contains(err.Error(), "unknown config key") {
			return false, false, nil
		}
		return false, false, err
	}
	b, ok := value.(bool)
	if !ok {
		return false, false, fmt.Errorf("config key %q is not boolean", key)
	}
	return b, true, nil
}

// StringValue reads a scalar string without adding it to the Workflows UI's
// mutable policy allowlist. Internal control-plane modules use it for existing
// product configuration such as roundtable.default.
func (s *Store) StringValue(key string) (string, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	root, err := s.read()
	if err != nil {
		return "", false, err
	}
	values := make(map[string]any)
	flatten(root, "", values)
	value, ok := values[key]
	if !ok {
		return "", false, nil
	}
	text, ok := value.(string)
	if !ok {
		return "", false, fmt.Errorf("config key %q is not a string", key)
	}
	return text, true, nil
}

func (s *Store) Set(key string, value any) error {
	return s.SetVersioned(key, value, "")
}

func (s *Store) SetVersioned(key string, value any, previousVersion string) error {
	if err := validateKeyValue(key, value); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	root, err := s.read()
	if err != nil {
		return err
	}
	if previousVersion != "" {
		current := configNodeVersion(mappingChild(root, key, false))
		if current != previousVersion {
			return fmt.Errorf("config version conflict (current %s)", current)
		}
	}
	parts := strings.Split(key, ".")
	if len(parts) == 0 {
		return errors.New("invalid config key")
	}
	node := root
	for _, part := range parts[:len(parts)-1] {
		if !safePart(part) {
			return errors.New("invalid config key")
		}
		node = mappingChild(node, part, true)
		if node == nil || node.Kind != yaml.MappingNode {
			return fmt.Errorf("config path %q conflicts with a non-mapping value", part)
		}
	}
	leaf := parts[len(parts)-1]
	if !safePart(leaf) {
		return errors.New("invalid config key")
	}
	valueNode := &yaml.Node{}
	if err := valueNode.Encode(value); err != nil {
		return fmt.Errorf("encode config value: %w", err)
	}
	setMappingChild(node, leaf, valueNode)
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(s.path), ".aimee.yaml.*.tmp")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if err := tmp.Chmod(0o600); err != nil {
		_ = tmp.Close()
		return err
	}
	encoder := yaml.NewEncoder(tmp)
	encoder.SetIndent(2)
	if err := encoder.Encode(root); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := encoder.Close(); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpPath, s.path)
}

func (s *Store) Version(key string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	root, err := s.read()
	if err != nil {
		return "", err
	}
	return configNodeVersion(mappingChild(root, key, false)), nil
}

func configNodeVersion(node *yaml.Node) string {
	if node == nil {
		node = &yaml.Node{Kind: yaml.SequenceNode, Tag: "!!seq"}
	}
	content, _ := yaml.Marshal(node)
	digest := sha256.Sum256(content)
	return hex.EncodeToString(digest[:])
}

func validateKeyValue(key string, value any) error {
	if key == "trigger_rules" {
		node := &yaml.Node{}
		if err := node.Encode(value); err != nil {
			return fmt.Errorf("encode trigger_rules: %w", err)
		}
		var rules []TriggerRule
		if err := node.Decode(&rules); err != nil {
			return fmt.Errorf("trigger_rules must be a list of rules: %w", err)
		}
		if len(rules) > MaxTriggerRules {
			return fmt.Errorf("trigger_rules has %d rules; maximum is %d", len(rules), MaxTriggerRules)
		}
		for i, rule := range rules {
			if rule.Source != "watch-dir" && rule.Source != "proposals" {
				return fmt.Errorf("trigger_rules[%d].source must be watch-dir or proposals", i)
			}
			if rule.Mode != "" && rule.Mode != "autonomous" && rule.Mode != "interactive" {
				return fmt.Errorf("trigger_rules[%d].mode must be autonomous or interactive", i)
			}
			event := rule.Event
			if event == "" {
				event = "docs/proposals/pending"
			}
			if !confinedTriggerDirectory(event) {
				return fmt.Errorf("trigger_rules[%d].event must be a repository-relative directory", i)
			}
			if strings.HasPrefix(strings.TrimSpace(rule.Schedule), "-") {
				return fmt.Errorf("trigger_rules[%d].schedule cannot start with '-'", i)
			}
			if rule.Pipeline.Template == "" || rule.Pipeline.Workspace == "" {
				return fmt.Errorf("trigger_rules[%d] needs pipeline.template and pipeline.workspace", i)
			}
			if !validTriggerWorkspace(rule.Pipeline.Workspace) {
				return fmt.Errorf("trigger_rules[%d].pipeline.workspace must be an absolute server path", i)
			}
			if rule.Pipeline.MaxSpendUSD < 0 || math.IsNaN(rule.Pipeline.MaxSpendUSD) || math.IsInf(rule.Pipeline.MaxSpendUSD, 0) {
				return fmt.Errorf("trigger_rules[%d].pipeline.max_spend_usd must be finite and non-negative", i)
			}
		}
		return nil
	}
	typeName, allowed := configurableTypes[key]
	if !allowed {
		return fmt.Errorf("config key %q is not editable", key)
	}
	switch typeName {
	case "int":
		n, ok := number(value)
		if !ok || n < 0 {
			return fmt.Errorf("%s must be a non-negative integer", key)
		}
		if bounds, bounded := configurableIntBounds[key]; bounded && (n < bounds.min || n > bounds.max) {
			return fmt.Errorf("%s must be between %d and %d", key, bounds.min, bounds.max)
		}
		if key == "autonomy.max_wall_secs" && n > 0 && n < MinAutonomyMaxWallSecs {
			return fmt.Errorf(
				"autonomy.max_wall_secs=%d is below the %d seconds a write-role delegate needs to run (%ds verifier reserve + %ds minimum run): every implement stage would refuse before starting, so no attempt could ever finish",
				n, MinAutonomyMaxWallSecs, writeVerifyReserveSecs, writeMinRunSecs)
		}
	case "bool":
		if _, ok := value.(bool); !ok {
			return fmt.Errorf("%s must be boolean", key)
		}
	}
	return nil
}

func confinedTriggerDirectory(directory string) bool {
	directory = strings.TrimSpace(directory)
	if strings.Contains(directory, "\\") || strings.IndexFunc(directory, func(r rune) bool { return r < 0x20 || r == 0x7f }) >= 0 {
		return false
	}
	clean := path.Clean(directory)
	return clean != "." && !path.IsAbs(clean) && clean != ".." && !strings.HasPrefix(clean, "../")
}

func validTriggerWorkspace(workspace string) bool {
	return workspace == strings.TrimSpace(workspace) && filepath.IsAbs(workspace) &&
		strings.IndexFunc(workspace, func(r rune) bool { return r < 0x20 || r == 0x7f }) < 0
}

func number(value any) (int64, bool) {
	switch n := value.(type) {
	case int:
		return int64(n), true
	case int64:
		return n, true
	case float64:
		if n == float64(int64(n)) {
			return int64(n), true
		}
	}
	return 0, false
}

func (s *Store) read() (*yaml.Node, error) {
	root := &yaml.Node{Kind: yaml.MappingNode, Tag: "!!map"}
	content, err := os.ReadFile(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return root, nil
	}
	if err != nil {
		return nil, err
	}
	var document yaml.Node
	if err := yaml.Unmarshal(content, &document); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}
	if len(document.Content) == 0 {
		return root, nil
	}
	if document.Content[0].Kind != yaml.MappingNode {
		return nil, errors.New("config root must be a mapping")
	}
	return document.Content[0], nil
}

func safePart(part string) bool {
	if part == "" {
		return false
	}
	for _, r := range part {
		if !(r == '_' || r == '-' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9') {
			return false
		}
	}
	return true
}

func mappingChild(node *yaml.Node, key string, create bool) *yaml.Node {
	for i := 0; i+1 < len(node.Content); i += 2 {
		if node.Content[i].Value == key {
			return node.Content[i+1]
		}
	}
	if !create {
		return nil
	}
	child := &yaml.Node{Kind: yaml.MappingNode, Tag: "!!map"}
	node.Content = append(node.Content, &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!str", Value: key}, child)
	return child
}

func setMappingChild(node *yaml.Node, key string, value *yaml.Node) {
	for i := 0; i+1 < len(node.Content); i += 2 {
		if node.Content[i].Value == key {
			node.Content[i+1] = value
			return
		}
	}
	node.Content = append(node.Content, &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!str", Value: key}, value)
}

func flatten(node *yaml.Node, prefix string, out map[string]any) {
	if node == nil || node.Kind != yaml.MappingNode {
		return
	}
	for i := 0; i+1 < len(node.Content); i += 2 {
		key := node.Content[i].Value
		if prefix != "" {
			key = prefix + "." + key
		}
		value := node.Content[i+1]
		if value.Kind == yaml.MappingNode {
			flatten(value, key, out)
			continue
		}
		var decoded any
		if value.Decode(&decoded) == nil {
			out[key] = decoded
		}
	}
}
