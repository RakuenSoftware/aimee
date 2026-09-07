package providers

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"testing"
	"time"
)

type modelTestVault struct {
	mu          sync.Mutex
	values      map[string]string
	unavailable bool
	writes      int
}

func (v *modelTestVault) Credential(_ context.Context, op, agent, cred, secret string) (string, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.unavailable {
		return "", errors.New("Vault unavailable")
	}
	slot := agent + "/" + cred
	if op == "set" {
		v.values[slot] = secret
		v.writes++
	}
	return v.values[slot], nil
}
func TestLocalModelIdentityUsesVaultAndSeparatesClientKeys(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	store := &modelTestVault{values: map[string]string{}}
	if err := ensureModelIdentity(ctx, store, root, "embedding", "aimee-embedder"); err != nil {
		t.Fatal(err)
	}
	var identity modelIdentity
	if json.Unmarshal([]byte(store.values["__model_embedding/api_key"]), &identity) != nil || identity.validate(time.Now()) != nil {
		t.Fatal("invalid persisted identity")
	}
	before, err := os.ReadFile(filepath.Join(root, "embedding/server/server.key"))
	if err != nil {
		t.Fatal(err)
	}
	if err = ensureModelIdentity(ctx, store, root, "embedding", "aimee-embedder"); err != nil {
		t.Fatal(err)
	}
	after, _ := os.ReadFile(filepath.Join(root, "embedding/server/server.key"))
	if string(before) != string(after) || store.writes != 1 {
		t.Fatal("restart rotated identity")
	}
	if _, err = os.Stat(filepath.Join(root, "embedding/server/client.key")); !os.IsNotExist(err) {
		t.Fatal("client key exposed to model container")
	}
	if info, err := os.Stat(filepath.Join(root, "embedding/client/client.key")); err != nil || info.Mode().Perm() != 0600 {
		t.Fatal("private key permissions")
	}
	for _, scenario := range []string{"corrupt", "wrong-host", "unavailable"} {
		t.Run(scenario, func(t *testing.T) {
			saved := store.values["__model_embedding/api_key"]
			defer func() { store.values["__model_embedding/api_key"] = saved; store.unavailable = false }()
			if scenario == "corrupt" {
				store.values["__model_embedding/api_key"] = "broken"
			}
			if scenario == "unavailable" {
				store.unavailable = true
			}
			host := "aimee-embedder"
			if scenario == "wrong-host" {
				host = "attacker.invalid"
			}
			if ensureModelIdentity(ctx, store, root, "embedding", host) == nil || store.writes != 1 {
				t.Fatal("invalid identity replaced/accepted")
			}
		})
	}
}

func TestCompetingModelBootstrapsKeepOneVaultIdentity(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux container bootstrap")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	home, root := t.TempDir(), t.TempDir()
	store := &modelTestVault{values: map[string]string{}}
	done := make(chan error, 8)
	for i := 0; i < 8; i++ {
		go func() {
			unlock, err := modelServicesLock(ctx, home)
			if err != nil {
				done <- err
				return
			}
			defer unlock()
			done <- ensureModelIdentity(ctx, store, root, "embedding", "aimee-embedder")
		}()
	}
	for i := 0; i < 8; i++ {
		if err := <-done; err != nil {
			t.Fatal(err)
		}
	}
	if store.writes != 1 {
		t.Fatalf("competing boots replaced the identity %d times", store.writes)
	}
	unlock, err := modelServicesLock(ctx, home)
	if err != nil {
		t.Fatal(err)
	}
	defer unlock()
	cancelled, stop := context.WithCancel(ctx)
	stop()
	if acquired, err := modelServicesLock(cancelled, home); err == nil {
		acquired()
		t.Fatal("competing bootstrap ignored deadline")
	}
}
