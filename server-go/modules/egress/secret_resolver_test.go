package egress

import (
	"context"
	"errors"
	"testing"
)

func TestVaultCredentialResolverBindsOpaqueHandleToCaller(t *testing.T) {
	seen := ""
	resolver := &vaultCredentialResolver{
		run: func(_ context.Context, name string) ([]byte, error) {
			seen = name
			return []byte("resolved-secret"), nil
		}}
	secret, err := resolver.Resolve(context.Background(), 712, "mcp:712")
	if err != nil || string(secret) != "resolved-secret" || seen != "AIMEE_MCP_712_TOKEN" {
		t.Fatalf("secret=%q name=%q err=%v", secret, seen, err)
	}
	clear(secret)
	for _, test := range []struct {
		principal uint32
		handle    string
	}{
		{713, "mcp:712"}, {712, "mcp:713"}, {71, "mcp:71"},
	} {
		if secret, err := resolver.Resolve(context.Background(), test.principal, test.handle); err == nil {
			clear(secret)
			t.Fatalf("cross-scope handle accepted: %+v", test)
		}
	}
}

func TestVaultCredentialResolverRejectsUnsafeBindingAndSecret(t *testing.T) {
	for _, name := range []string{"MCP_TOKEN", "AIMEE_DB2_URL", "AIMEE_MCP_TOKEN\nOTHER", "AIMEE_MCP_lower_TOKEN"} {
		if validMCPVaultName(name) {
			t.Fatalf("unsafe MCP vault name accepted: %q", name)
		}
	}
	resolver := &vaultCredentialResolver{
		run: func(context.Context, string) ([]byte, error) { return nil, errors.New("unavailable") }}
	if _, err := resolver.Resolve(context.Background(), 712, "mcp:712"); err == nil {
		t.Fatal("failed credential helper was accepted")
	}
}
