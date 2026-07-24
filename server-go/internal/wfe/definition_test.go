package wfe

import (
	"os"
	"path/filepath"
	"testing"
)

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
