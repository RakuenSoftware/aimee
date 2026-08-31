package delegates

import (
	"sort"
	"strings"
)

// What a delegate is ALLOWED to do.
//
// One set, resolved once when the delegate is created, carried for the life of
// the run. Nothing downstream re-derives it: the mount reads it, the tool
// allowlist reads it, the API reads it. A permission computed twice is a
// permission that can disagree with itself, and this system has proved that
// twice already -- a task type re-classified from keywords at every context
// refresh, and a role list that drifted from its own duplicate.
//
// Reads are implicit and ungranted. Any delegate may read the checkout it was
// given and query what aimee knows; a permission only ever authorises leaving
// something behind or reaching further out.
//
// THREE THINGS MAKE UP A PERMISSION, and all three are declarable:
//
//	name               what is authorised        e.g. repo_write
//	scope              which objects it covers   e.g. only these two repos
//	enforcement point  where it is evaluated     e.g. the mount
//
// The vocabulary is OPEN. A role may be composed of permissions this module has
// never heard of, bound to enforcement points an operator supplies. What this
// module guarantees is the built-in set below and the points those are bound to.
// Anything else is the operator's to make work -- see
// docs/DELEGATE_ROLE_PERMISSIONS.md -- and this module's only obligations are
// to carry it faithfully and to say plainly when nothing is enforcing it.

// EnforcementPoint names where a permission is actually evaluated.
//
// A permission is only as strong as its choke point: it holds exactly when every
// path to the effect passes through that point. The kernel and the network
// boundary are unavoidable; an in-process check is not, which is why a
// tool-layer rule cannot bind a delegate that holds a shell.
type EnforcementPoint string

const (
	// EnforceMount: the container's filesystem mount. Unavoidable, because the
	// kernel decides. The only honest home for anything about writing files.
	EnforceMount EnforcementPoint = "mount"

	// EnforceAPI: the server-side handler that performs the effect. Unavoidable
	// for anything that must come back through aimee -- memory, the index, docs
	// -- including from inside a container holding the control socket.
	EnforceAPI EnforcementPoint = "api"

	// EnforceTools: the tool dispatch allowlist, which decides what tools exist
	// for a delegate.
	//
	// WEAKER THAN IT LOOKS: it binds a delegate that has no shell, and only
	// that one. A delegate holding `shell` reaches the same effects by running
	// commands, so anything that must hold against a shell belongs at the mount
	// or the API instead.
	EnforceTools EnforcementPoint = "tools"

	// EnforceNone: declared, and nothing evaluates it.
	//
	// A permission that reads like a control and is not one. It is reported
	// rather than tolerated: a role template saying `deploy: false` while
	// nothing enforces `deploy` is worse than saying nothing at all.
	EnforceNone EnforcementPoint = ""
)

// The built-in permissions: the set this module guarantees.
const (
	PermTools          = "tools"
	PermShell          = "shell"
	PermRepoWrite      = "repo_write"
	PermKnowledgeWrite = "knowledge_write"
)

// builtinPermissions is the shipped vocabulary and, critically, where each one
// is enforced BY DEFAULT.
//
// An operator may rebind any of these to a point of their own, and may add
// permissions that are not here at all. Both are supported; neither is
// guaranteed by us.
var builtinPermissions = map[string]EnforcementPoint{
	// May call tools at all. Without it a delegate is a pure text transform:
	// prompt in, prose out.
	PermTools: EnforceTools,

	// May run commands (bash, execute_script). NOT implied by tools -- a
	// reviewer reads files, greps and searches without ever holding a shell.
	PermShell: EnforceTools,

	// May modify the checkout. Enforced by the MOUNT: once shell is granted the
	// absence of write_file stops nothing, so a delegate without this must be
	// handed its workspace read-only.
	PermRepoWrite: EnforceMount,

	// May mutate what aimee knows -- memory, the code index, docs. Enforced at
	// the API, where the mutation lands. The guard this replaces matched the
	// substring "aimee " in shell text, which a delegate with a shell defeats by
	// writing a script, by word-splitting, or by curling the control socket it
	// was handed.
	PermKnowledgeWrite: EnforceAPI,
}

// BuiltinPermissionNames lists the guaranteed vocabulary, sorted.
func BuiltinPermissionNames() []string {
	out := make([]string, 0, len(builtinPermissions))
	for name := range builtinPermissions {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// DefaultEnforcementPoint reports where a permission is enforced when the
// operator has not said otherwise. EnforceNone for anything not built in.
func DefaultEnforcementPoint(permission string) EnforcementPoint {
	return builtinPermissions[normalisePermission(permission)]
}

func normalisePermission(name string) string {
	return strings.ToLower(strings.TrimSpace(name))
}

// A Grant is one permission, as held by a role.
type Grant struct {
	// Name of the permission. May be one this module does not know.
	Name string

	// Scopes narrows the grant to particular objects. Empty is unrestricted.
	//
	// A scope is only real where the enforcement point can SEE the object.
	// repo_write narrowed to two repositories works because the mount decides
	// which are read-write. Narrowing a tool-layer permission by directory does
	// not bind a shell, so such a scope is a statement about what gets mounted,
	// and is enforced there or not at all.
	Scopes []string

	// EnforcedAt is where this grant is evaluated. Empty means the built-in
	// default applies; with no built-in either, nothing enforces it.
	EnforcedAt EnforcementPoint
}

// Permissions is everything a role may do.
type Permissions struct {
	grants map[string]Grant
}

// Has reports whether a permission is held at all, ignoring narrowing.
//
// Use it only where the object is not yet known -- deciding whether to mount a
// workspace read-write at all, say. Where an object IS known, Allows is the
// question that matters, and Has would say yes to something the scope forbids.
func (p Permissions) Has(permission string) bool {
	_, ok := p.grants[normalisePermission(permission)]
	return ok
}

// Allows reports whether a permission covers this particular object.
//
// An unscoped grant covers everything. A scoped grant covers exactly what it
// lists: matching is exact, because a prefix rule would make /srv/repo also
// grant /srv/repo-secrets, and nobody writing the first means the second.
func (p Permissions) Allows(permission, object string) bool {
	grant, ok := p.grants[normalisePermission(permission)]
	if !ok {
		return false
	}
	if len(grant.Scopes) == 0 {
		return true
	}
	for _, scope := range grant.Scopes {
		if scope == object {
			return true
		}
	}
	return false
}

// Scopes returns what a permission was narrowed to, empty when unrestricted.
func (p Permissions) Scopes(permission string) []string {
	return p.grants[normalisePermission(permission)].Scopes
}

// EnforcedAt reports where a held permission is evaluated: the operator's choice
// if they made one, otherwise the built-in default, otherwise EnforceNone.
func (p Permissions) EnforcedAt(permission string) EnforcementPoint {
	grant, ok := p.grants[normalisePermission(permission)]
	if !ok {
		return EnforceNone
	}
	if grant.EnforcedAt != EnforceNone {
		return grant.EnforcedAt
	}
	return DefaultEnforcementPoint(grant.Name)
}

// Unenforced lists held permissions that nothing evaluates, sorted.
//
// THIS IS THE ONE THING WE OWE AN OPERATOR WHO ADDS THEIR OWN. A permission with
// no enforcement point is a promise nobody keeps: it reads like a control in a
// role definition and changes nothing at runtime. Callers surface this loudly
// when a delegate is created rather than quietly treating it as granted or as
// denied, both of which are guesses.
func (p Permissions) Unenforced() []string {
	var out []string
	for name := range p.grants {
		if p.EnforcedAt(name) == EnforceNone {
			out = append(out, name)
		}
	}
	sort.Strings(out)
	return out
}

// Names lists the held permissions, sorted.
func (p Permissions) Names() []string {
	out := make([]string, 0, len(p.grants))
	for name := range p.grants {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// RoleDefinition is a role as declared at runtime, composed of permissions.
//
// A definition REPLACES the built-in rather than adding to it: "composed of
// these permissions" has to mean the list is the whole answer, or reading a
// definition would not tell you what the role may do.
type RoleDefinition struct {
	Grants []Grant
}

// scopeIsEvaluated says whether anything actually narrows this built-in
// permission by object.
//
// Only repo_write does: the write decision matches the repository the caller
// named. `knowledge_write` and `tools` carry scopes that nothing reads, and
// `shell` cannot be scoped at the tool layer at all -- a shell goes wherever the
// filesystem lets it.
//
// A scope nobody evaluates is worse than an unenforced permission. An unenforced
// permission is reported; a scope that is silently ignored reads as a narrowing
// the operator can point at, while the delegate holds the permission in full.
// So it is refused, and the refusal says which permissions can be scoped.
func scopeIsEvaluated(permission string) bool {
	return normalisePermission(permission) == PermRepoWrite
}

// ScopableBuiltins names the built-in permissions a scope actually narrows.
// Operator-defined permissions are not listed: what their points evaluate is
// theirs to say.
func ScopableBuiltins() []string {
	return []string{PermRepoWrite}
}

// ResolveRolePermissions answers what a role may do, preferring a definition
// supplied at runtime over the built-in role table.
//
// `defined` is nil when no runtime definition exists. The distinction matters: a
// definition granting nothing is a deliberate powerless role, while no
// definition at all falls back to the built-in -- and a name that is neither
// holds nothing, because an unrecognised role is a question nobody answered.
func ResolveRolePermissions(role string, defined *RoleDefinition) Permissions {
	out := Permissions{grants: map[string]Grant{}}
	if defined == nil {
		for _, name := range builtinRolePermissions[canonicalRole(role)] {
			out.grants[name] = Grant{Name: name}
		}
		return out
	}
	// A name appearing twice is refused by ParseRoleDefinition, so there is no
	// contradiction to resolve here. Merging them used to widen to the union of
	// their scopes with an unscoped mention winning outright, which turned a
	// repeated line into a grant over every object it had been scoped away from.
	// Choosing the permissive reading of a mistake is not a decision this should
	// be making; the operator is told and picks.
	for _, grant := range defined.Grants {
		name := normalisePermission(grant.Name)
		if name == "" {
			continue
		}
		grant.Name = name
		out.grants[name] = grant
	}
	return out
}

// builtinRolePermissions is what each shipped role may do.
//
// These reproduce the behaviour the scattered predicates produced before this
// existed -- RoleIsWrite, RoleEnablesToolsByDefault, the per-role toolsets and
// the "current code only" flag -- extracted from those functions rather than
// written from memory, so adopting them changes no delegate's powers.
//
// A role absent from this table holds NOTHING. An unrecognised role is an open
// question, and answering it with "reads only" is still an answer nobody made.
var builtinRolePermissions = map[string][]string{
	// Writers: the only roles that may change the repository.
	"code":     {PermTools, PermShell, PermRepoWrite, PermKnowledgeWrite},
	"refactor": {PermTools, PermShell, PermRepoWrite, PermKnowledgeWrite},

	// Runs things, changes nothing in the checkout.
	"execute": {PermTools, PermShell, PermKnowledgeWrite},

	// Inspects the current code with a shell, and leaves no trace in aimee's
	// knowledge: its whole job is to report what is there now.
	"diagnose": {PermTools, PermShell},

	// Judges an artefact. Reads and searches, never runs anything -- which is
	// what "read-only reviewer" has to mean once a shell can write files.
	"review": {PermTools},

	// Checks, with a shell for running the thing it is checking.
	"validate": {PermTools, PermShell, PermKnowledgeWrite},

	// Looks things up.
	"search": {PermTools, PermKnowledgeWrite},

	// Novel-mode inspection roles.
	"continuity": {PermTools, PermKnowledgeWrite},
	"beat-check": {PermTools, PermKnowledgeWrite},

	// Pure text transforms: prompt in, prose out, no tools at all.
	"explain":   {PermKnowledgeWrite},
	"draft":     {PermKnowledgeWrite},
	"summarize": {PermKnowledgeWrite},
	"format":    {PermKnowledgeWrite},
	"plan":      {PermKnowledgeWrite},
	"reason":    {PermKnowledgeWrite},
}

// RoleHasPermission reports whether a built-in role holds a permission.
func RoleHasPermission(role, permission string) bool {
	return ResolveRolePermissions(role, nil).Has(permission)
}
