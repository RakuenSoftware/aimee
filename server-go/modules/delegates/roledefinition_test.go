package delegates

import (
	"strings"
	"testing"
)

const definedRole = `---
max_turns: 40
permissions:
  - tools
  - name: repo_write
    scopes: [/srv/repo-a, /srv/repo-b]
    enforced_at: mount
  - name: deploy
    enforced_at: deploy-gate
---

You are a deploy delegate.
`

func TestARoleAnOperatorWroteIsReadWhole(t *testing.T) {
	definition, err := ParseRoleDefinition(definedRole)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if definition == nil || len(definition.Grants) != 3 {
		t.Fatalf("definition = %+v", definition)
	}

	permissions := ResolveRolePermissions("deployer", definition)
	if !permissions.Has(PermTools) {
		t.Error("the bare form grants the permission it names")
	}
	if !permissions.Allows(PermRepoWrite, "/srv/repo-a") ||
		permissions.Allows(PermRepoWrite, "/srv/repo-c") {
		t.Error("a scoped grant covers what it lists and nothing else")
	}
	if permissions.EnforcedAt("deploy") != "deploy-gate" {
		t.Errorf("an operator's own enforcement point is carried: %q",
			permissions.EnforcedAt("deploy"))
	}
}

// No permissions block means the role was not defined, and the built-in table
// answers. That is not the same as a definition granting nothing.
func TestNoPermissionsBlockIsNotAnEmptyDefinition(t *testing.T) {
	definition, err := ParseRoleDefinition("---\nmax_turns: 40\n---\n\nbody\n")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if definition != nil {
		t.Fatalf("definition = %+v, want nil", definition)
	}

	if !ResolveRolePermissions("code", nil).Has(PermRepoWrite) {
		t.Error("an undefined role falls back to what ships")
	}
	empty := &RoleDefinition{}
	if ResolveRolePermissions("code", empty).Has(PermRepoWrite) {
		t.Error("a definition granting nothing is a deliberate powerless role")
	}
}

// A line this cannot read fails the parse. Dropping it would leave an operator
// believing they granted a power they did not.
func TestAnUnreadableLineIsAnErrorNotAnOmission(t *testing.T) {
	for name, text := range map[string]string{
		"unknown field":    "permissions:\n  - name: tools\n    scoped_to: /srv\n",
		"nameless":         "permissions:\n  - enforced_at: mount\n",
		"field first":      "permissions:\n    enforced_at: mount\n",
		"block scopes":     "permissions:\n  - name: repo_write\n    scopes:\n      - /srv\n",
		"unclosed scopes":  "permissions:\n  - name: repo_write\n    scopes: [/srv\n",
		"empty scope item": "permissions:\n  - name: repo_write\n    scopes: [/srv, ]\n",
	} {
		if _, err := ParseRoleDefinition(text); err == nil {
			t.Errorf("%s: parsed without complaint", name)
		}
	}
}

func TestTheBlockEndsWhereTheNextKeyBegins(t *testing.T) {
	definition, err := ParseRoleDefinition("permissions:\n  - tools\nmax_turns: 40\n")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(definition.Grants) != 1 || definition.Grants[0].Name != PermTools {
		t.Errorf("grants = %+v", definition.Grants)
	}
}

// An empty scope list is refused rather than quietly read as unscoped, which is
// what the wire would turn it into: the difference between "scoped to nothing"
// and "scoped to everything" is every object in the system.
func TestAnEmptyScopeListIsRefused(t *testing.T) {
	_, err := ParseRoleDefinition("permissions:\n  - name: repo_write\n    scopes: []\n")
	if err == nil {
		t.Fatal("an empty scope list parsed")
	}
	if !strings.Contains(err.Error(), "leave it out") {
		t.Errorf("the error should say what to do instead: %v", err)
	}
}

// A permission listed twice is refused rather than merged.
//
// The merge that used to happen chose the permissive reading: an unscoped
// mention beat a scoped one, so repeating `repo_write` under a scoped grant
// silently granted every repository. A contradiction in a permission block is
// the operator's to resolve, and they cannot resolve one nobody told them about.
func TestAPermissionListedTwiceIsRefused(t *testing.T) {
	_, err := ParseRoleDefinition(
		"permissions:\n  - name: repo_write\n    scopes: [/srv/repo-a]\n  - repo_write\n")
	if err == nil {
		t.Fatal("a repeated permission parsed")
	}
	if !strings.Contains(err.Error(), "listed twice") ||
		!strings.Contains(err.Error(), PermRepoWrite) {
		t.Errorf("the error should name the permission: %v", err)
	}

	// Case is the same name, not two.
	if _, err := ParseRoleDefinition("permissions:\n  - tools\n  - name: TOOLS\n"); err == nil {
		t.Error("a repeat in different case parsed")
	}

	// Two DIFFERENT permissions are of course fine.
	if _, err := ParseRoleDefinition("permissions:\n  - tools\n  - shell\n"); err != nil {
		t.Errorf("two distinct permissions: %v", err)
	}
}
