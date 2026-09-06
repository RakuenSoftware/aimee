package providers

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
)

const maxRosterBytes = 1 << 20
const maxModels = 16

type object = map[string]any

func str(m object, key string) string { v, _ := m[key].(string); return v }
func number(m object, key string) float64 {
	switch v := m[key].(type) {
	case float64:
		return v
	case int:
		return float64(v)
	case json.Number:
		n, _ := v.Float64()
		return n
	}
	return 0
}
func boolean(m object, key string, fallback bool) bool {
	v, ok := m[key].(bool)
	if !ok {
		return fallback
	}
	return v
}
func copyObject(m object) object {
	b, _ := json.Marshal(m)
	var out object
	_ = json.Unmarshal(b, &out)
	return out
}
func rows(m object, key string) []object {
	out := []object{}
	if a, ok := m[key].([]any); ok {
		for _, v := range a {
			if row, ok := v.(map[string]any); ok {
				out = append(out, row)
			}
		}
	}
	if a, ok := m[key].([]object); ok {
		return a
	}
	return out
}

type Store struct {
	home string
	mu   sync.Mutex
}

func NewStore(home string) (*Store, error) {
	if home == "" || !filepath.IsAbs(home) {
		return nil, errors.New("providers: absolute home required")
	}
	return &Store{home: home}, nil
}
func (s *Store) path() (string, error) {
	path := filepath.Join(s.home, "models.json")
	if _, err := os.Stat(path); err == nil {
		return path, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	legacy := filepath.Join(s.home, "agents.json")
	if _, err := os.Stat(legacy); err == nil {
		return legacy, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	return path, nil
}
func array(root object, key string) ([]object, error) {
	v, exists := root[key]
	if !exists {
		return []object{}, nil
	}
	a, ok := v.([]any)
	if !ok {
		return nil, fmt.Errorf("%s must be an array", key)
	}
	result := make([]object, 0, len(a))
	for _, v := range a {
		row, ok := v.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("invalid %s row", key)
		}
		result = append(result, row)
	}
	return result, nil
}
func (s *Store) load(path string) (object, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return object{"models": []object{}, "providers": []object{}}, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, maxRosterBytes+1))
	if err != nil {
		return nil, err
	}
	if len(b) > maxRosterBytes {
		return nil, errors.New("roster is too large")
	}
	var root object
	if json.Unmarshal(b, &root) != nil || root == nil {
		return nil, errors.New("invalid provider/model configuration")
	}
	root, err = normalizeRoot(root)
	if err != nil {
		return nil, err
	}
	return s.defaults(root), nil
}

func normalizeRoot(root object) (object, error) {
	key := "models"
	if _, ok := root[key]; !ok {
		key = "agents"
	}
	models, err := array(root, key)
	if err != nil {
		return nil, err
	}
	if len(models) > maxModels {
		return nil, errors.New("maximum number of models exceeded")
	}
	models, err = expandModels(models)
	if err != nil {
		return nil, err
	}
	connections, err := array(root, "providers")
	if err != nil {
		return nil, err
	}
	byName := map[string]object{}
	for _, p := range connections {
		if str(p, "name") == "" || str(p, "provider") == "" {
			return nil, errors.New("invalid saved provider")
		}
		if byName[str(p, "name")] != nil {
			return nil, errors.New("duplicate saved provider")
		}
		if ref := str(p, "api_key_ref"); ref != "" && !strings.HasPrefix(ref, "$") {
			return nil, errors.New("saved provider contains a literal credential")
		}
		byName[str(p, "name")] = p
	}
	for _, m := range models {
		name := str(m, "registration")
		if name == "" {
			name = str(m, "name")
		}
		if name == "" {
			return nil, errors.New("model name required")
		}
		if byName[name] == nil {
			p := object{"name": name, "provider": str(m, "provider"), "endpoint": str(m, "endpoint"), "auth_type": str(m, "auth_type")}
			if str(p, "provider") == "" {
				p["provider"] = "openai"
			}
			if str(p, "auth_type") == "" {
				p["auth_type"] = "bearer"
			}
			for _, field := range connectionFields {
				if v, ok := m[field]; ok {
					p[field] = v
				}
			}
			if key := str(m, "api_key"); strings.HasPrefix(key, "$") {
				p["api_key_ref"] = key
			}
			connections = append(connections, p)
			byName[name] = p
		}
		// Model identity is independent from protocol and endpoint, including when
		// two accounts share the same vendor URL.
		m["registration"] = name
		if str(m, "api_key") == "" || strings.HasPrefix(str(m, "api_key"), "$") {
			applyConnection(m, byName[name])
		}
		if err := normalizeModel(m); err != nil {
			return nil, err
		}
	}
	root["models"] = models
	delete(root, "agents")
	root["providers"] = connections
	return root, nil
}

var connectionFields = []string{"provider", "endpoint", "auth_type", "auth_cmd", "backend", "cli_kind", "cli_cmd", "is_server_hosted", "session_reuse"}

// transaction serializes every roster mutation, including other processes using
// the compatibility snapshot writer. The lock inode is stable across renames.
func (s *Store) transaction(write bool, fn func(object) (object, error)) (object, error) {
	return s.transactionBeforeCommit(write, fn, nil)
}

func (s *Store) transactionBeforeCommit(write bool, fn func(object) (object, error), beforeCommit func() (func(), error)) (object, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := os.MkdirAll(s.home, 0700); err != nil {
		return nil, err
	}
	lock, err := os.OpenFile(filepath.Join(s.home, ".providers.lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	defer lock.Close()
	if err = syscall.Flock(int(lock.Fd()), syscall.LOCK_EX); err != nil {
		return nil, err
	}
	defer syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
	path, err := s.path()
	if err != nil {
		return nil, err
	}
	root, err := s.load(path)
	if err != nil {
		return nil, err
	}
	reply, err := fn(root)
	if err != nil || !write {
		return reply, err
	}
	for _, model := range rows(root, "models") {
		if err := normalizeModel(model); err != nil {
			return nil, err
		}
	}
	root, err = normalizeRoot(copyObject(root))
	if err != nil {
		return nil, err
	}
	root = s.defaults(root)
	if reply != nil {
		reply["revision"] = revision(root)
	}
	// Credential references may be persisted; plaintext credentials never may.
	for _, m := range rows(root, "models") {
		if key := str(m, "api_key"); key != "" && !strings.HasPrefix(key, "$") {
			return nil, errors.New("literal model credential must be migrated to Vault")
		}
	}
	b, err := json.MarshalIndent(root, "", "  ")
	if err != nil {
		return nil, err
	}
	if len(b)+1 > maxRosterBytes {
		return nil, errors.New("roster is too large")
	}
	b = append(b, '\n')
	f, err := os.CreateTemp(s.home, ".models-*")
	if err != nil {
		return nil, err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if _, err = f.Write(b); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	committed := false
	var rollback func()
	if beforeCommit != nil {
		rollback, err = beforeCommit()
		if err != nil {
			return nil, err
		}
	}
	defer func() {
		if !committed && rollback != nil {
			rollback()
		}
	}()
	if err = os.Rename(tmp, path); err != nil {
		return nil, err
	}
	committed = true
	dir, err := os.Open(s.home)
	if err != nil {
		return nil, err
	}
	defer dir.Close()
	if err = dir.Sync(); err != nil {
		return nil, err
	}
	return reply, nil
}
func find(items []object, name string) object {
	for _, item := range items {
		if str(item, "name") == name {
			return item
		}
	}
	return nil
}
func validText(value string, max int) bool {
	return len(value) < max && !strings.ContainsAny(value, "\x00\r\n")
}
func owns(p, m object) bool {
	name := str(m, "registration")
	if name == "" {
		name = str(m, "name")
	}
	return name == str(p, "name")
}
func applyConnection(m, p object) {
	for _, field := range connectionFields {
		if field == "cli_cmd" && str(m, field) != "" {
			continue
		}
		if v, ok := p[field]; ok {
			m[field] = v
		} else {
			delete(m, field)
		}
	}
	m["registration"] = str(p, "name")
	delete(m, "api_key")
	if ref := str(p, "api_key_ref"); ref != "" {
		m["api_key"] = ref
	}
}

var nativeModelFields = map[string]bool{
	"api_key":                   true,
	"auth_cmd":                  true,
	"auth_type":                 true,
	"backend":                   true,
	"catalog_provider":          true,
	"catalog_provider_explicit": true,
	"cli_cmd":                   true,
	"cli_idle_timeout_ms":       true,
	"cli_kind":                  true,
	"context_window":            true,
	"cost_tier":                 true,
	"enabled":                   true,
	"endpoint":                  true,
	"exec_roles":                true,
	"exec_system_prompt":        true,
	"extra_headers":             true,
	"fallback_model":            true,
	"inject_respond_tool":       true,
	"is_server_hosted":          true,
	"max_output":                true,
	"max_parallel":              true,
	"max_scope":                 true,
	"max_tokens":                true,
	"max_turns":                 true,
	"middleware":                true,
	"model":                     true,
	"name":                      true,
	"personas":                  true,
	"price_cached_per_mtok":     true,
	"price_in_per_mtok":         true,
	"price_out_per_mtok":        true,
	"primary_only":              true,
	"provider":                  true,
	"recommended_sampling":      true,
	"registration":              true,
	"roles":                     true,
	"session_reuse":             true,
	"tier_price_exempt":         true,
	"timeout_ms":                true,
	"tools_enabled":             true,
}
