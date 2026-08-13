package delegates

import "testing"

// Every built-in role. Kept explicit so a role added to the alias table without
// permissions fails here rather than silently getting none.
var builtInRoles = []string{
	"review", "validate", "diagnose", "code", "refactor", "explain", "draft",
	"execute", "summarize", "format", "search", "reason", "plan",
	"continuity", "beat-check",
}

// --- the declarations must reproduce today's behaviour ----------------------

func TestPermissionsReproduceTheOldWriteRule(t *testing.T) {
	for _, role := range builtInRoles {
		if got, want := RoleHasPermission(role, PermRepoWrite), RoleIsWrite(role); got != want {
			t.Errorf("%q: repo_write = %v, RoleIsWrite = %v", role, got, want)
		}
	}
}

// The tools default was a second list of these same roles until it became this
// permission, and then had no caller at all and was deleted. This pins the
// answers that list gave; the equivalence itself was proved role by role while
// both existed.
func TestPermissionsCarryTheOldToolsAnswers(t *testing.T) {
	withTools := map[string]bool{
		"code": true, "refactor": true, "execute": true, "validate": true,
		"diagnose": true, "review": true, "search": true,
		"continuity": true, "beat-check": true,
	}
	for _, role := range builtInRoles {
		if got := RoleHasPermission(role, PermTools); got != withTools[role] {
			t.Errorf("%q: tools = %v, want %v", role, got, withTools[role])
		}
	}
	if RoleHasPermission("", PermTools) {
		t.Error("no role holds nothing")
	}
}

// Knowledge writes were previously denied by the "current code only" flag, whose
// real job was "may read aimee's state but not mutate it". That predicate has
// been deleted, so this pins the answers it used to give: the equivalence itself
// was proved role by role while both existed.
func TestPermissionsCarryTheOldKnowledgeAnswers(t *testing.T) {
	confined := map[string]bool{"review": true, "diagnose": true}
	for _, role := range builtInRoles {
		got, want := RoleHasPermission(role, PermKnowledgeWrite), !confined[role]
		if got != want {
			t.Errorf("%q: knowledge_write = %v, want %v", role, got, want)
		}
	}
}

// Shell was never a predicate: it was implied by which toolset a role resolved
// to. These are the roles whose toolset carried bash/execute_script.
func TestShellMatchesTheToolsetsThatCarriedBash(t *testing.T) {
	withShell := map[string]bool{
		"code": true, "refactor": true, "execute": true, // full_stack
		"diagnose": true, // current_code
		"validate": true, // validate
	}
	for _, role := range builtInRoles {
		if got := RoleHasPermission(role, PermShell); got != withShell[role] {
			t.Errorf("%q: shell = %v, want %v", role, got, withShell[role])
		}
	}
}

func TestAReviewerHasToolsButNoShellAndNoWrite(t *testing.T) {
	if !RoleHasPermission("review", PermTools) {
		t.Error("a reviewer needs tools to read what it reviews")
	}
	for _, permission := range []string{PermShell, PermRepoWrite, PermKnowledgeWrite} {
		if RoleHasPermission("review", permission) {
			t.Errorf("a reviewer must not hold %s", permission)
		}
	}
}

// Collapsing tools and shell would hand every reviewer a shell, which is the one
// thing a read-only role must not have.
func TestToolsDoesNotImplyShell(t *testing.T) {
	found := false
	for _, role := range builtInRoles {
		if RoleHasPermission(role, PermTools) && !RoleHasPermission(role, PermShell) {
			found = true
		}
	}
	if !found {
		t.Fatal("if no role has tools without a shell the two permissions are the same thing")
	}
}

func TestAliasesResolveBeforePermissionsAreRead(t *testing.T) {
	for _, pair := range [][2]string{
		{"reviewer", "review"}, {"implement", "code"}, {"inspect", "diagnose"},
		{"verifier", "validate"}, {"test", "validate"},
	} {
		a := ResolveRolePermissions(pair[0], nil).Names()
		b := ResolveRolePermissions(pair[1], nil).Names()
		if len(a) != len(b) {
			t.Errorf("%q and %q are the same role: %v vs %v", pair[0], pair[1], a, b)
			continue
		}
		for i := range a {
			if a[i] != b[i] {
				t.Errorf("%q and %q differ: %v vs %v", pair[0], pair[1], a, b)
				break
			}
		}
	}
}

func TestAnUnknownRoleHoldsNoPermissions(t *testing.T) {
	for _, role := range []string{"", "wat", "custom-operator-role"} {
		if got := ResolveRolePermissions(role, nil).Names(); len(got) != 0 {
			t.Errorf("ResolveRolePermissions(%q) = %v, want none", role, got)
		}
	}
}

func TestEveryBuiltInRoleIsDeclared(t *testing.T) {
	for _, role := range builtInRoles {
		if _, ok := builtinRolePermissions[role]; !ok {
			t.Errorf("%q has no permission declaration", role)
		}
	}
}

// --- scope -----------------------------------------------------------------

func TestABuiltInRoleIsUnrestricted(t *testing.T) {
	perms := ResolveRolePermissions("code", nil)
	if !perms.Allows(PermRepoWrite, "/anywhere/at/all") {
		t.Error("an unscoped grant covers every object")
	}
	if len(perms.Scopes(PermRepoWrite)) != 0 {
		t.Error("a built-in declares no scopes")
	}
}

// The case that motivated scopes: a role that may write, but only to named
// repositories.
func TestRepoWriteCanBeNarrowedToParticularRepositories(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{
		{Name: PermTools},
		{Name: PermShell},
		{Name: PermRepoWrite, Scopes: []string{"/srv/repo-a", "/srv/repo-b"}},
	}}
	perms := ResolveRolePermissions("custom", &defined)

	if !perms.Has(PermRepoWrite) {
		t.Fatal("the permission is held")
	}
	for _, allowed := range []string{"/srv/repo-a", "/srv/repo-b"} {
		if !perms.Allows(PermRepoWrite, allowed) {
			t.Errorf("%s is in scope", allowed)
		}
	}
	for _, denied := range []string{"/srv/repo-c", "/srv", ""} {
		if perms.Allows(PermRepoWrite, denied) {
			t.Errorf("%s is not in scope", denied)
		}
	}
	if !perms.Allows(PermShell, "anything") {
		t.Error("shell was declared without a scope and stays unrestricted")
	}
}

// Prefix matching would make /srv/repo grant /srv/repo-secrets.
func TestAScopeDoesNotMatchByPrefix(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{
		{Name: PermRepoWrite, Scopes: []string{"/srv/repo"}},
	}}
	perms := ResolveRolePermissions("custom", &defined)
	for _, outside := range []string{"/srv/repo-secrets", "/srv/repo/sub"} {
		if perms.Allows(PermRepoWrite, outside) {
			t.Errorf("%s is a different object; matching is exact", outside)
		}
	}
}

// Has ignores narrowing on purpose, so it must not be used where the object is
// known. Pinned because the two read as equivalent.
func TestHasIgnoresScopeAndAllowsDoesNot(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{
		{Name: PermRepoWrite, Scopes: []string{"/srv/only-this"}},
	}}
	perms := ResolveRolePermissions("custom", &defined)
	if !perms.Has(PermRepoWrite) {
		t.Fatal("held")
	}
	if perms.Allows(PermRepoWrite, "/srv/something-else") {
		t.Fatal("but not for this object")
	}
}

// --- roles defined at runtime ----------------------------------------------

func TestADefinitionReplacesTheBuiltIn(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{{Name: PermTools}}}
	perms := ResolveRolePermissions("code", &defined)
	if !perms.Has(PermTools) {
		t.Fatal("tools was declared")
	}
	if perms.Has(PermRepoWrite) || perms.Has(PermShell) {
		t.Error("the definition is the whole answer, not an addition to the built-in")
	}
}

func TestAnEmptyDefinitionGrantsNothing(t *testing.T) {
	empty := RoleDefinition{}
	if got := ResolveRolePermissions("code", &empty).Names(); len(got) != 0 {
		t.Errorf("an empty definition grants nothing, got %v", got)
	}
	if got := ResolveRolePermissions("code", nil).Names(); len(got) == 0 {
		t.Error("no definition falls back to the built-in")
	}
}

// --- enforcement points ------------------------------------------------------

// Where each built-in permission is evaluated is part of what this module
// guarantees, so it is pinned rather than left to a comment.
func TestBuiltInPermissionsAreBoundToTheirPoints(t *testing.T) {
	for permission, want := range map[string]EnforcementPoint{
		PermTools:          EnforceTools,
		PermShell:          EnforceTools,
		PermRepoWrite:      EnforceMount,
		PermKnowledgeWrite: EnforceAPI,
	} {
		if got := DefaultEnforcementPoint(permission); got != want {
			t.Errorf("%s defaults to %q, want %q", permission, got, want)
		}
	}
}

// The two permissions that must hold against a delegate with a shell are the two
// that are NOT enforced at the tool layer. If either moves to `tools`, the
// guarantee is gone and this says so.
func TestWhatMustHoldAgainstAShellIsNotEnforcedByTheToolLayer(t *testing.T) {
	for _, permission := range []string{PermRepoWrite, PermKnowledgeWrite} {
		if DefaultEnforcementPoint(permission) == EnforceTools {
			t.Errorf("%s cannot be enforced at the tool layer: a shell walks around it",
				permission)
		}
	}
}

// An operator may bind a permission to a point of their own.
func TestAnOperatorCanRebindAnEnforcementPoint(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{
		{Name: PermRepoWrite, EnforcedAt: EnforcementPoint("site-policy-daemon")},
	}}
	perms := ResolveRolePermissions("custom", &defined)
	if got := perms.EnforcedAt(PermRepoWrite); got != "site-policy-daemon" {
		t.Errorf("EnforcedAt = %q, want the operator's point", got)
	}
	if len(perms.Unenforced()) != 0 {
		t.Error("a permission bound to an operator's point is enforced, as far as we know")
	}
}

// An operator may add a permission this module has never heard of.
func TestAnOperatorCanAddTheirOwnPermission(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{
		{Name: "deploy", Scopes: []string{"staging"}, EnforcedAt: EnforcementPoint("deploy-gate")},
	}}
	perms := ResolveRolePermissions("custom", &defined)
	if !perms.Has("deploy") {
		t.Fatal("an unknown name is carried, not dropped")
	}
	if !perms.Allows("deploy", "staging") || perms.Allows("deploy", "production") {
		t.Error("scopes work the same for an operator's permission")
	}
	if len(perms.Unenforced()) != 0 {
		t.Errorf("it names its enforcement point: %v", perms.Unenforced())
	}
}

// THE FAILURE MODE THIS MODEL EXISTS TO SURFACE: a permission declared with
// nothing to enforce it. It reads like a control in a role definition and
// changes nothing at runtime, so it must be reported rather than assumed either
// way.
func TestAPermissionWithNoEnforcementPointIsReported(t *testing.T) {
	defined := RoleDefinition{Grants: []Grant{
		{Name: PermTools},
		{Name: "deploy"}, // no point, and not one we ship
	}}
	perms := ResolveRolePermissions("custom", &defined)

	unenforced := perms.Unenforced()
	if len(unenforced) != 1 || unenforced[0] != "deploy" {
		t.Fatalf("Unenforced() = %v, want [deploy]", unenforced)
	}
	if perms.EnforcedAt("deploy") != EnforceNone {
		t.Error("nothing evaluates it")
	}
	// It is still HELD: reporting is not the same as silently denying, which
	// would be its own guess.
	if !perms.Has("deploy") {
		t.Error("the declaration is carried faithfully; only its enforcement is missing")
	}
}

// A built-in permission is never unenforced, because we ship its point.
func TestBuiltInPermissionsAreNeverUnenforced(t *testing.T) {
	for _, role := range builtInRoles {
		if got := ResolveRolePermissions(role, nil).Unenforced(); len(got) != 0 {
			t.Errorf("%q has unenforced permissions %v", role, got)
		}
	}
}
