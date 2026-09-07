package kb

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
	"testing"
)

func TestIdentityAndConflictingRole(t *testing.T) {
	home := t.TempDir()
	original, err := identity.Ensure(home, "kb")
	if err != nil {
		t.Fatal(err)
	}
	handler, err := NewHandler(home)
	if err != nil {
		t.Fatal(err)
	}
	raw, status := handler(bus.ModuleInvocation{StageID: StageIdentity}, []byte(`{"operation":"identity"}`))
	var got identity.Identity
	if status != bus.ModuleStatusOK || json.Unmarshal(raw, &got) != nil || got != original {
		t.Fatal("wrong identity")
	}
	for _, request := range []string{`{"operation":"switch","role":"server"}`, `{"operation":"identity","role":"kb"}`, `{"operation":"identity"}{}`} {
		if _, status := handler(bus.ModuleInvocation{StageID: StageIdentity}, []byte(request)); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("mutation/malformed identity accepted")
		}
	}
	different := t.TempDir()
	if _, err := identity.Ensure(different, "server"); err != nil {
		t.Fatal(err)
	}
	if _, err := NewHandler(different); err == nil {
		t.Fatal("conflicting role accepted")
	}
}
