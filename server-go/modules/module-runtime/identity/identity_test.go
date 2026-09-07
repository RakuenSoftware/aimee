package identity

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func TestFirstBootIdentitySurvivesRestartsAndRejectsRoleChange(t *testing.T) {
	for _, role := range []string{Server, KB} {
		t.Run(role, func(t *testing.T) {
			home := t.TempDir()
			first, err := Ensure(home, role)
			if err != nil {
				t.Fatal(err)
			}
			again, err := Ensure(home, role)
			if err != nil || first != again {
				t.Fatal("restart changed identity", err)
			}
			other := Server
			if role == Server {
				other = KB
			}
			before, _ := os.ReadFile(filepath.Join(home, FileName))
			if _, err := Ensure(home, other); err == nil {
				t.Fatal("role change accepted")
			}
			after, _ := os.ReadFile(filepath.Join(home, FileName))
			if string(before) != string(after) {
				t.Fatal("identity rewritten")
			}
			canonical, _ := json.Marshal(first)
			canonical = append(canonical, '\n')
			if string(before) != string(canonical) {
				t.Fatal("core's canonical identity encoding changed")
			}
		})
	}
}
func TestConcurrentFirstBootConvergesOnOneRoleAndID(t *testing.T) {
	home := t.TempDir()
	var wg sync.WaitGroup
	results := make(chan Identity, 32)
	for n := 0; n < 32; n++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			role := Server
			if n%2 == 1 {
				role = KB
			}
			got, err := Ensure(home, role)
			if err == nil {
				results <- got
			}
		}(n)
	}
	wg.Wait()
	close(results)
	stored, err := Read(home)
	if err != nil {
		t.Fatal(err)
	}
	count := 0
	for got := range results {
		count++
		if got != stored {
			t.Fatal("multiple identities admitted")
		}
	}
	if count != 16 {
		t.Fatalf("got %d successful creators; expected only the 16 callers of the winning role", count)
	}
}
func TestInvalidExistingIdentityIsNeverReplaced(t *testing.T) {
	for _, variant := range []string{"corrupt", "writable", "symlink", "trailing", "unknown-field"} {
		t.Run(variant, func(t *testing.T) {
			home := t.TempDir()
			id, err := Ensure(home, Server)
			if err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(home, FileName)
			if err := os.Chmod(path, 0600); err != nil {
				t.Fatal(err)
			}
			data, _ := json.Marshal(id)
			switch variant {
			case "corrupt":
				data = []byte("{broken")
			case "trailing":
				data = append(data, []byte("{}")...)
			case "unknown-field":
				data = append(data[:len(data)-1], []byte(",\"switchable\":true}")...)
			}
			if err := os.WriteFile(path, data, 0600); err != nil {
				t.Fatal(err)
			}
			if variant != "writable" {
				if err := os.Chmod(path, 0444); err != nil {
					t.Fatal(err)
				}
			}
			if variant == "symlink" {
				target := path + ".real"
				if err := os.Rename(path, target); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(target, path); err != nil {
					t.Fatal(err)
				}
			}
			if _, err := Ensure(home, KB); err == nil {
				t.Fatal("invalid identity treated as first boot")
			}
			got, _ := os.ReadFile(path)
			if string(got) != string(data) {
				t.Fatal("existing identity changed")
			}
		})
	}
}
