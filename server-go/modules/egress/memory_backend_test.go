package egress

import (
	"context"
	"net/url"
	"testing"
)

func TestRetiredMemoryBackendHasNoNetworkOrCredentialAdmission(t *testing.T) {
	target, _ := url.Parse("http://legacy-memory:8097/v1/rank")
	for _, ref := range []uint32{MemoryClientRef, ProvidersClientRef, GitClientRef} {
		for _, method := range []string{"GET", "POST", "DELETE"} {
			if callerPurposeAllowed(ref, Request{Purpose: "memory-backend", Method: method}, target) {
				t.Fatal("retired provider still admitted", ref, method)
			}
		}
	}
	calls := 0
	resolver := vaultCredentialResolver{run: func(context.Context, string) ([]byte, error) { calls++; return []byte("fixture"), nil }}
	if _, err := resolver.Resolve(context.Background(), MemoryClientRef, "memory-backend"); err == nil || calls != 0 {
		t.Fatal("retired credential still resolved", err, calls)
	}
}
