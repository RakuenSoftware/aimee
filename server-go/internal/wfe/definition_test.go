package wfe

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWatchDirectoryTriggerParamsAreValidatedWhenWorkflowIsSaved(t *testing.T) {
	definition := func(params string) []byte {
		return []byte(fmt.Sprintf(`
name: watcher
start: watch
nodes:
  - id: watch
    block: trigger.watch-dir
    params:
%s
`, params))
	}

	for _, tc := range []struct {
		name, params, want string
	}{
		{"workspace-type", "      workspace: 12", `param "workspace" must be a string`},
		{"workspace-relative", "      workspace: repos/demo", "absolute server path"},
		{"directory-type", "      dir: 12", `param "dir" must be a string`},
		{"directory-traversal", "      dir: ../private", "confined repository-relative directory"},
		{"directory-backslash", `      dir: 'docs\\private'`, "confined repository-relative directory"},
		{"ref-option", "      ref: --all", "start with '-'"},
		{"mode", "      mode: manual", "autonomous or interactive"},
		{"spend-negative", "      max_spend_usd: -1", "finite and non-negative"},
		{"spend-type", "      max_spend_usd: lots", "finite and non-negative"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := ParseDefinition(definition(tc.params))
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error=%v, want %q", err, tc.want)
			}
		})
	}

	for _, params := range []string{
		"      workspace: /srv/repos/demo\n      dir: docs/proposals/pending\n      ref: testing\n      mode: interactive\n      max_spend_usd: 1.5",
		// A definition without a workspace is intentionally saved but disarmed;
		// the operator can fill it in later without an invalid workflow blocking boot.
		"      dir: docs/proposals/pending",
	} {
		if _, err := ParseDefinition(definition(params)); err != nil {
			t.Fatalf("valid watch trigger rejected: %v", err)
		}
	}
}

func TestCurrentBuildWorkflowParses(t *testing.T) {
	path := filepath.Join("..", "..", "..", "config", "workflows", "build.yaml")
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	def, err := ParseDefinition(content)
	if err != nil {
		t.Fatal(err)
	}
	if def.Name != "build" || def.Version == "" {
		t.Fatalf("unexpected definition: name=%q version=%q", def.Name, def.Version)
	}
	gate, ok := def.Node("plan_gate")
	if !ok || gate.OnFail != "plan" {
		t.Fatalf("plan gate not preserved: %+v", gate)
	}
}

func TestDefinitionRejectsMissingProducer(t *testing.T) {
	_, err := ParseDefinition([]byte(`
name: broken
start: gate
nodes:
  - id: gate
    block: gate.roundtable
    in:
      src: missing.out
`))
	if err == nil {
		t.Fatal("missing producer was accepted")
	}
}

// A roundtable review that names no roundtable used to resolve an implicit
// panel nobody configured. The name is part of the block contract now, so a
// workflow that omits it must not parse at all.
func TestRoundtableGateWithoutANamedRoundtableIsRejected(t *testing.T) {
	definition := []byte(`
name: unnamed
start: plan
nodes:
  - id: plan
    block: author.plan
    in: {proposal: plan.out}
    next: gate
  - id: gate
    block: gate.roundtable
    in: {src: plan.out}
    params: {panel: {required: [qa]}}
`)
	_, err := ParseDefinition(definition)
	if err == nil {
		t.Fatal("gate.roundtable parsed with no roundtable named")
	}
	if !strings.Contains(err.Error(), `requires param "roundtable"`) {
		t.Fatalf("unexpected error: %v", err)
	}
	// A blank name is the same omission, not a different one.
	_, err = ParseDefinition(bytes.Replace(definition,
		[]byte("params: {panel:"), []byte(`params: {roundtable: "  ", panel:`), 1))
	if err == nil || !strings.Contains(err.Error(), `requires param "roundtable"`) {
		t.Fatalf("blank roundtable name accepted: %v", err)
	}
}

// Every workflow the image ships must name a roundtable that the image also
// ships, or the gate parks on a preset that does not exist.
func TestShippedWorkflowsNameAShippedRoundtable(t *testing.T) {
	workflowDir := filepath.Join("..", "..", "..", "config", "workflows")
	entries, err := os.ReadDir(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	gates := 0
	for _, entry := range entries {
		if filepath.Ext(entry.Name()) != ".yaml" {
			continue
		}
		content, err := os.ReadFile(filepath.Join(workflowDir, entry.Name()))
		if err != nil {
			t.Fatal(err)
		}
		def, err := ParseDefinition(content)
		if err != nil {
			t.Fatalf("shipped workflow %s does not validate: %v", entry.Name(), err)
		}
		for _, node := range def.Nodes {
			if node.Block != "gate.roundtable" {
				continue
			}
			gates++
			name, _ := node.Params["roundtable"].(string)
			preset := filepath.Join("..", "..", "..", "config", "roundtables", name+".json")
			if _, err := os.Stat(preset); err != nil {
				t.Errorf("%s node %q names roundtable %q, which ships no preset: %v",
					entry.Name(), node.ID, name, err)
			}
		}
	}
	if gates == 0 {
		t.Fatal("no shipped roundtable gates found; this guard would pass vacuously")
	}
}
