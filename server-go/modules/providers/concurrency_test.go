package providers

import (
	"context"
	"errors"
	configclient "github.com/JBailes/aimee/server-go/config"
	"testing"
)

type recoveryConfig struct {
	fail   bool
	limits map[string]int
}

func (c *recoveryConfig) StringValue(string) (string, bool, error) { return "", false, nil }
func (c *recoveryConfig) Set(string, any) error                    { return nil }
func (c *recoveryConfig) SetModelConcurrency(m configclient.ModelConcurrencyMutation) error {
	if c.fail {
		return errors.New("config unavailable")
	}
	c.limits[m.Model] = m.Limit
	return nil
}
func (c *recoveryConfig) RemoveModelConcurrency(id string) error {
	if c.fail {
		return errors.New("config unavailable")
	}
	delete(c.limits, id)
	return nil
}
func TestConcurrencyProjectionRecoversAndSnapshotRevisionRemainsUsable(t *testing.T) {
	m, home, vault := manager(t)
	cfg := &recoveryConfig{fail: true, limits: map[string]int{}}
	m.SetConfig(cfg)
	_, err := m.Manage(context.Background(), Request{Operation: "model.add", Arguments: object{"args": []string{"local", "http://localhost/v1", "m", "--max-parallel", "2"}}})
	if err == nil {
		t.Fatal("unavailable config was hidden")
	}
	store, _ := NewStore(home)
	m = NewManager(store, vault)
	cfg.fail = false
	m.SetConfig(cfg)
	call(t, m, "model.list", nil)
	if cfg.limits["m"] != 2 {
		t.Fatal(cfg.limits)
	}
	snapshot := call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	rows(snapshot, "models")[0]["max_parallel"] = 4
	reply := call(t, m, "snapshot.save", object{"config": snapshot, "expected_revision": str(snapshot, "revision")})
	snapshot = call(t, m, "snapshot.load", nil)["config"].(map[string]any)
	if cfg.limits["m"] != 4 || str(reply, "revision") != str(snapshot, "revision") {
		t.Fatal(reply, snapshot, cfg.limits)
	}
	call(t, m, "model.remove", object{"args": []string{"local"}})
	if _, ok := cfg.limits["m"]; ok {
		t.Fatal(cfg.limits)
	}
}
