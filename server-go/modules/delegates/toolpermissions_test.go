package delegates

import (
	"strings"
	"testing"
)

func denies(t *testing.T, role string, definition *RoleDefinition, tool string) bool {
	t.Helper()
	for _, denied := range DeniedTools(ResolveRolePermissions(role, definition)) {
		if denied == tool {
			return true
		}
	}
	return false
}

// A toolset says what a role is for; it cannot grant what the role does not
// hold. `review` resolves to a toolset with no bash in it either, but the
// permission is what makes that true rather than a coincidence of two lists.
func TestAPermissionWithheldWithholdsItsTools(t *testing.T) {
	for _, tool := range []string{"bash", "execute_script", "run_background_process"} {
		if !denies(t, "review", nil, tool) {
			t.Errorf("review holds no shell, so %q must be withheld", tool)
		}
	}
	for _, tool := range []string{"write_file", "edit_file", "git_commit", "git_push"} {
		if !denies(t, "review", nil, tool) {
			t.Errorf("review holds no repo_write, so %q must be withheld", tool)
		}
	}
	if denies(t, "code", nil, "write_file") || denies(t, "code", nil, "bash") {
		t.Error("code holds both, so it keeps its tools")
	}
}

// The case the clamp exists for: a role whose NAME resolves to a toolset
// carrying write_file, defined by an operator without repo_write.
func TestADefinedRoleDoesNotKeepToolsItsNameImplies(t *testing.T) {
	definition, err := ParseRoleDefinition("permissions:\n  - tools\n  - shell\n")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if !denies(t, "code", definition, "write_file") {
		t.Error("a defined role without repo_write must not keep write_file")
	}
	if !denies(t, "code", definition, "git_push") {
		t.Error("nor git_push, which lands the same changes by another route")
	}
	if denies(t, "code", definition, "bash") {
		t.Error("it does hold shell, so bash stays")
	}
}

// Reading is implicit, so an unclassified tool is not privileged. If this ever
// flips, every delegate loses every tool nobody thought to classify.
func TestAnUnclassifiedToolNeedsNoPermission(t *testing.T) {
	if ToolRequiresPermission("read_file") != "" {
		t.Error("read_file needs no permission beyond tools")
	}
	if ToolRequiresPermission("a_tool_that_does_not_exist") != "" {
		t.Error("an unknown tool must not be treated as privileged")
	}
	if denies(t, "review", nil, "read_file") {
		t.Error("a read-only role keeps its read tools")
	}
}

// A scope on a permission nothing narrows by object is refused where the
// operator can see it, rather than carried to no effect.
func TestAScopeOnAnUnevaluatedPermissionIsRefused(t *testing.T) {
	_, err := ParseRoleDefinition("permissions:\n  - name: knowledge_write\n    scopes: [notes]\n")
	if err == nil {
		t.Fatal("a knowledge_write scope parsed")
	}
	if !strings.Contains(err.Error(), "cannot be scoped") ||
		!strings.Contains(err.Error(), PermRepoWrite) {
		t.Errorf("the error should name what CAN be scoped: %v", err)
	}

	// repo_write still can be, and an operator's own permission is their business.
	if _, err := ParseRoleDefinition(
		"permissions:\n  - name: repo_write\n    scopes: [/srv/a]\n"); err != nil {
		t.Errorf("repo_write is scopable: %v", err)
	}
	if _, err := ParseRoleDefinition(
		"permissions:\n  - name: deploy\n    scopes: [staging]\n    enforced_at: gate\n"); err != nil {
		t.Errorf("an operator's own permission may be scoped however they enforce it: %v", err)
	}
}
