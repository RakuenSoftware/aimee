package vaultresource

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os/exec"
)

// VaultResources uses the core Vault owner's pipe protocol. It does not read
// key files, derive a KEK, or introduce another credential store.
type VaultResources struct{ Home string }

func (r VaultResources) Credential(ctx context.Context, op, agent, cred, secret string) (string, error) {
	request, err := json.Marshal(map[string]any{"operation": op, "agent": agent, "credential": cred, "secret": secret})
	if err != nil {
		return "", err
	}
	defer clear(request)
	cmd := exec.CommandContext(ctx, "/usr/local/bin/aimee-server", "--module-vault-resource")
	cmd.Env = []string{"AIMEE_HOME=" + r.Home, "PATH=/usr/local/bin:/usr/bin:/bin"}
	cmd.Stdin = bytes.NewReader(request)
	reply, err := cmd.Output()
	defer clear(reply)
	if err != nil {
		return "", errors.New("credential storage unavailable")
	}
	var result struct {
		Status string `json:"status"`
		Value  string `json:"value"`
	}
	if json.Unmarshal(reply, &result) != nil || result.Status != "ok" {
		return "", errors.New("credential storage unavailable")
	}
	return result.Value, nil
}
