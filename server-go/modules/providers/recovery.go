package providers

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
)

func revision(root object) string {
	b, _ := json.Marshal(root)
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// Deletions are recorded in the same atomic roster commit as removing their
// owner. Retrying cleanup after a crash is safe, and a name cannot be reused
// until every old credential has been removed.
func queueCleanup(root object, name string) {
	pending := rows(root, "credential_cleanup")
	for _, credential := range []string{"api_key", "codex_oauth_token", "codex_account_id", "oauth"} {
		pending = append(pending, object{"name": name, "credential": credential})
	}
	root["credential_cleanup"] = pending
}
func (m *Manager) recoverCredentials(ctx context.Context) error {
	dirty := false
	_, err := m.store.transaction(false, func(root object) (object, error) {
		dirty = len(rows(root, "credential_cleanup")) > 0
		for _, model := range rows(root, "models") {
			key := str(model, "api_key")
			dirty = dirty || key != "" && !strings.HasPrefix(key, "$")
		}
		return nil, nil
	})
	if err != nil || !dirty {
		return err
	}
	if m.resources == nil {
		return errors.New("credential service unavailable")
	}
	_, err = m.store.transaction(true, func(root object) (object, error) {
		for _, item := range rows(root, "credential_cleanup") {
			if _, err := m.resources.Credential(ctx, "delete", str(item, "name"), str(item, "credential"), ""); err != nil {
				return nil, err
			}
		}
		delete(root, "credential_cleanup")
		for _, model := range rows(root, "models") {
			key := str(model, "api_key")
			if key == "" || strings.HasPrefix(key, "$") {
				continue
			}
			name := str(model, "registration")
			if name == "" {
				name = str(model, "name")
			}
			if _, err := m.resources.Credential(ctx, "set", name, "api_key", key); err != nil {
				return nil, err
			}
			delete(model, "api_key")
		}
		return nil, nil
	})
	return err
}
