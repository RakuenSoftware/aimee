package memory

import "testing"

func TestContentGate(t *testing.T) {
	clean := scanContent("documented in /src/cache.go", 128)
	if clean.SensitiveStatus != 0 || !clean.Evidence || clean.Classification != "normal" {
		t.Fatalf("clean = %#v", clean)
	}
	secret := scanContent("prefix token=abc suffix", 128)
	if secret.SensitiveStatus != 1 || secret.Redacted != "prefix [REDACTED] suffix" ||
		secret.Classification != "restricted" {
		t.Fatalf("secret = %#v", secret)
	}
	tooSmall := scanContent("prefix token=abc", 8)
	if tooSmall.SensitiveStatus != 2 {
		t.Fatalf("tooSmall = %#v", tooSmall)
	}
	blocked := scanContent("-----BEGIN PRIVATE KEY-----", 128)
	if blocked.Classification != "blocked" {
		t.Fatalf("blocked = %#v", blocked)
	}
	ephemeral := scanContent("currently 12 files", 128)
	if !ephemeral.Ephemeral {
		t.Fatalf("ephemeral = %#v", ephemeral)
	}
}
