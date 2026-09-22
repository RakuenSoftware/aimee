package memory

import (
	"encoding/json"
	"strings"
	"testing"
)

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

func TestScreenContentCommands(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		client := clientForHandler(t, NewHandler(nil, WithDataStore(placement, nil)))
		for _, tt := range []struct {
			content           string
			capacity          int
			verdict, redacted string
		}{
			{"ordinary text", 64, "allow", ""},
			{"token=first password=second", 128, "redact", "[REDACTED] [REDACTED]"},
			{"token=first token=second ghp_abcdefghijklmnop", 128, "redact", "[REDACTED] [REDACTED] [REDACTED]"},
			{"AKIA0123456789ABCDEF 123-45-6789", 128, "redact", "[REDACTED] [REDACTED]"},
			{"-----BEGIN RSA PRIVATE KEY-----\nsecret body\n-----END RSA PRIVATE KEY-----", 1024, "reject", ""},
			{"prefix token=x suffix 🦊", 20, "reject", ""},
			{"token=x 🦊", 64, "redact", "[REDACTED] 🦊"},
		} {
			args, _ := json.Marshal(map[string]any{"content": tt.content, "capacity": tt.capacity})
			r := runPublicCommand(t, client, "screen_content", string(args))
			if r["status"] != "ok" || r["verdict"] != tt.verdict || r["redacted"] != tt.redacted {
				t.Fatal(placement, tt, r)
			}
			if tt.verdict == "redact" && (strings.Contains(r["redacted"].(string), "first") || strings.Contains(r["redacted"].(string), "second")) {
				t.Fatal(r)
			}
		}
		if r := runPublicCommand(t, client, "screen_content", `{"content":false}`); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
}
