// Native compatibility tests exercise the real Go owner through a pipe, without
// starting a production bus or opening the developer's Vault.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"os"
	"path/filepath"

	"github.com/JBailes/aimee/server-go/modules/providers"
)

type fixtureVault struct{ home string }

func (v fixtureVault) Credential(_ context.Context, op, account, credential, secret string) (string, error) {
	path := filepath.Join(v.home, ".provider-test-vault.json")
	values := map[string]string{}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &values)
	}
	key := account + ":" + credential
	switch op {
	case "get":
		return values[key], nil
	case "has":
		if values[key] != "" {
			return "1", nil
		}
		return "0", nil
	case "set":
		values[key] = secret
	case "delete":
		delete(values, key)
	}
	b, _ := json.Marshal(values)
	return "", os.WriteFile(path, b, 0600)
}
func main() {
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 65536), 2<<20)
	encoder := json.NewEncoder(os.Stdout)
	for scanner.Scan() {
		var input struct {
			Home    string            `json:"home"`
			Env     map[string]string `json:"env"`
			Request providers.Request `json:"request"`
		}
		if json.Unmarshal(scanner.Bytes(), &input) != nil {
			os.Exit(1)
		}
		for name, value := range input.Env {
			if value == "" {
				_ = os.Unsetenv(name)
			} else {
				_ = os.Setenv(name, value)
			}
		}
		store, err := providers.NewStore(input.Home)
		var reply map[string]any
		if err == nil {
			manager := providers.NewManager(store, fixtureVault{input.Home})
			reply, err = manager.Manage(context.Background(), input.Request)
		}
		if err != nil {
			reply = map[string]any{"status": "error", "kind": "unavailable", "message": err.Error()}
		}
		if encoder.Encode(reply) != nil {
			os.Exit(1)
		}
	}
}
