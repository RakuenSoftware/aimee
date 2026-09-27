package tools

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"os"
	"path/filepath"
	"testing"
)

func TestGovernedResourceUsesTrustedToolAndExactTarget(t *testing.T) {
	root := t.TempDir()
	a, e := DescribeGovernedAction("write_file", []byte(`{"path":"a","content":"same","side_effect":"read"}`), root)
	if e != nil || a.Class != "file_write" || a.Destination != "file:"+filepath.Join(root, "a") {
		t.Fatal(a, e)
	}
	b, e := DescribeGovernedAction("write_file", []byte(`{"path":"b","content":"same","side_effect":"read"}`), root)
	if e != nil || a.Destination == b.Destination || a.PayloadDigest == b.PayloadDigest {
		t.Fatal("different destination collapsed", b, e)
	}
	if e = os.Symlink(root, filepath.Join(root, "alias")); e != nil {
		t.Fatal(e)
	}
	alias, e := DescribeGovernedAction("write_file", []byte(`{"path":"alias/a","content":"same"}`), root)
	if e != nil || alias.Destination != a.Destination {
		t.Fatal("path alias lost object identity", alias, e)
	}
	for _, raw := range []string{`{"path":"a","path":"b"}`, `{"path":"a\u0000b"}`, `{"path":"a"} {}`, `[]`, `{"path":1}`, `{"path":"missing/a"}`} {
		if _, e := DescribeGovernedAction("write_file", []byte(raw), root); e == nil {
			t.Fatal("ambiguous/unavailable target accepted", raw)
		}
	}
	if _, e := DescribeGovernedAction("untrusted:publish", []byte(`{"path":"a","side_effect":"read"}`), root); e == nil {
		t.Fatal("unresolved tool got authority")
	}
}

func TestGovernedFileVerificationExactObject(t *testing.T) {
	root := t.TempDir()
	raw := []byte(`{"path":"target","content":"expected"}`)
	resource, err := DescribeGovernedAction("write_file", raw, root)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(root, "other"), []byte("expected"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err = VerifyGovernedAction("write_file", raw, root, resource.Destination, resource.PayloadDigest); err == nil {
		t.Fatal("equal bytes at wrong object verified")
	}
	if err = os.WriteFile(filepath.Join(root, "target"), []byte("wrong"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err = VerifyGovernedAction("write_file", raw, root, resource.Destination, resource.PayloadDigest); err == nil {
		t.Fatal("wrong bytes verified")
	}
	if err = os.WriteFile(filepath.Join(root, "target"), []byte("expected"), 0600); err != nil {
		t.Fatal(err)
	}
	proof, err := VerifyGovernedAction("write_file", raw, root, resource.Destination, resource.PayloadDigest)
	if err != nil || proof.State != "effect_confirmed" || proof.ObjectVersion == "" {
		t.Fatal(proof, err)
	}
	if _, err = VerifyGovernedAction("write_file", raw, root, "file:"+filepath.Join(root, "other"), resource.PayloadDigest); err == nil {
		t.Fatal("changed destination verified")
	}
	if err = os.Remove(filepath.Join(root, "target")); err != nil {
		t.Fatal(err)
	}
	if err = os.Symlink(filepath.Join(root, "other"), filepath.Join(root, "target")); err != nil {
		t.Fatal(err)
	}
	if _, err = VerifyGovernedAction("write_file", raw, root, resource.Destination, resource.PayloadDigest); err == nil {
		t.Fatal("redirected target verified")
	}
}

func TestGovernedFileClassesComeFromOperatorPolicy(t *testing.T) {
	root := t.TempDir()
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	policy, _ := json.Marshal(map[string]any{"actions": map[string]any{"sensitive_path_prefixes": []string{filepath.Join(root, "private")}, "published_path_prefixes": []string{filepath.Join(root, "public")}}})
	if err := os.WriteFile(filepath.Join(home, "policy.json"), policy, 0600); err != nil {
		t.Fatal(err)
	}
	for _, directory := range []string{"private", "public"} {
		if err := os.Mkdir(filepath.Join(root, directory), 0700); err != nil {
			t.Fatal(err)
		}
	}
	for _, tc := range []struct{ tool, path, class string }{{"read_file", "private/input", "sensitive_read"}, {"write_file", "public/output", "external_publish"}, {"write_file", "private/input", "file_write"}} {
		raw, _ := json.Marshal(map[string]string{"path": tc.path, "side_effect": "read_only"})
		r, err := DescribeGovernedAction(tc.tool, raw, root)
		if err != nil || r.Class != tc.class {
			t.Fatal(tc, r, err)
		}
	}
}

func TestGovernedResourceRejectsDanglingLink(t *testing.T) {
	root := t.TempDir()
	if err := os.Symlink(filepath.Join(root, "missing"), filepath.Join(root, "alias")); err != nil {
		t.Fatal(err)
	}
	if _, err := DescribeGovernedAction("write_file", []byte(`{"path":"alias","content":"x"}`), root); err == nil {
		t.Fatal("dangling link admitted as a new file")
	}
}

func TestGovernedResourceWireRequiresAuthenticatedHost(t *testing.T) {
	args, _ := json.Marshal(map[string]any{"tool": "write_file", "arguments": map[string]string{"path": "target"}, "directory": t.TempDir()})
	wire, err := bus.EncodeCommandWithContext("describe", args, bus.CommandContext{Authenticated: true, Principal: "alice"})
	if err != nil {
		t.Fatal(err)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageActionResource}, wire); status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageActionResource, PrincipalRef: 73}, wire); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("plugin supplied host resource authority")
	}
	wire, _ = bus.EncodeCommand("describe", args)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageActionResource}, wire); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("missing authenticated context accepted")
	}
}
