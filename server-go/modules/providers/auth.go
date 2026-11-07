package providers

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

func (m *Manager) resolveKey(ctx context.Context, p object) (string, error) {
	if str(p, "auth_type") == "none" {
		return "", nil
	}
	// Explicit named connections never fall back to another account's Vault key.
	// Legacy environment references remain explicit operator configuration.
	if m.resources == nil {
		return "", errors.New("credential service unavailable")
	}
	if ref := str(p, "api_key_ref"); strings.HasPrefix(ref, "$") {
		return m.resources.Credential(ctx, "get", "environment", strings.Trim(ref[1:], "{}"), "")
	}
	key, err := m.resources.Credential(ctx, "get", str(p, "name"), "api_key", "")
	if err != nil || key != "" {
		return key, err
	}
	if envs, ok := p["env_vars"].([]string); ok {
		for _, env := range envs {
			if v, err := m.resources.Credential(ctx, "get", "environment", env, ""); err != nil || v != "" {
				return v, err
			}
		}
	}
	if envs, ok := p["env_vars"].([]any); ok {
		for _, env := range envs {
			if s, ok := env.(string); ok {
				if v, err := m.resources.Credential(ctx, "get", "environment", s, ""); err != nil || v != "" {
					return v, err
				}
			}
		}
	}
	return "", nil
}

type credentialChange struct{ name, secret string }

// Validate and fsync the new roster before changing Vault. Restore previous
// credentials if Vault or the roster rename fails; plaintext never enters the
// roster, a journal, process arguments, or an environment variable.
func (m *Manager) commitCredentials(ctx context.Context, changes []credentialChange) (func(), error) {
	previous := []credentialChange{}
	rollback := func() {
		recovery, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		for i := len(previous) - 1; i >= 0; i-- {
			item := previous[i]
			op := "set"
			if item.secret == "" {
				op = "delete"
			}
			_, _ = m.resources.Credential(recovery, op, item.name, "api_key", item.secret)
		}
	}
	for _, item := range changes {
		old, err := m.resources.Credential(ctx, "get", item.name, "api_key", "")
		if err != nil {
			rollback()
			return nil, err
		}
		previous = append(previous, credentialChange{name: item.name, secret: old})
		if _, err = m.resources.Credential(ctx, "set", item.name, "api_key", item.secret); err != nil {
			rollback()
			return nil, err
		}
	}
	return rollback, nil
}

// Resolve the same selected, refresh-aware credential/account snapshot as serving.
// This private resource response is never a provider roster or model payload.
type codexCredentialSnapshot struct {
	Token         string `json:"token"`
	Account       string `json:"account_id"`
	Source        string `json:"source_digest"`
	ClientVersion string `json:"client_version"`
}

func (m *Manager) resolveCodexSnapshot(ctx context.Context, p object) (codexCredentialSnapshot, error) {
	if m.resources == nil {
		return codexCredentialSnapshot{}, errors.New("credential service unavailable")
	}
	document, err := m.resources.Credential(ctx, "get", str(p, "name"), "codex_oauth_credentials", "")
	if err != nil {
		return codexCredentialSnapshot{}, err
	}
	var pair codexCredentialSnapshot
	if json.Unmarshal([]byte(document), &pair) != nil || pair.Token == "" || len(pair.Source) != 64 || strings.ContainsAny(pair.Token+pair.Account, "\r\n") {
		return codexCredentialSnapshot{}, errors.New("Codex OAuth credential unavailable; re-authenticate this provider")
	}
	return pair, nil
}

// Preserve the serving/probe helper's credential-only contract. Catalog callers
// additionally require the declared compatibility version from the same pair.
func (m *Manager) resolveCodex(ctx context.Context, p object) (string, string, error) {
	pair, err := m.resolveCodexSnapshot(ctx, p)
	return pair.Token, pair.Account, err
}
