package delegates

import "sort"

// Which permission a tool needs before a delegate may be handed it.
//
// A toolset says what a role is FOR; a permission says what it may do. The two
// are answered in different places and the toolset is the older of them, so a
// role whose toolset carries `write_file` was handed it whatever its permissions
// said. This is the clamp: the tools below are withheld from a delegate that
// does not hold the permission named, whatever toolset it resolved to.
//
// It lives here because it is a rule about permissions, not about toolsets. The
// caller resolves the set once and gets back the list of tools that set denies,
// so dispatch never asks a second question.
//
// Tools not listed need no permission beyond `tools` itself. That is the honest
// default: reading is implicit, and a tool nobody has classified must not be
// silently treated as privileged (it would vanish from every delegate) or
// silently treated as safe when it writes. Adding a tool that leaves something
// behind means adding it here.
var toolPermissions = map[string]string{
	// Running commands. The background-process tools are shell by another name:
	// they start and read from a command, so withholding `shell` while leaving
	// them would be a boundary with a door in it.
	"bash":                      PermShell,
	"execute_script":            PermShell,
	"run_background_process":    PermShell,
	"get_background_output":     PermShell,
	"kill_background_process":   PermShell,
	"list_background_processes": PermShell,

	// Changing the checkout. git_commit and the rest land the same changes by a
	// different route, which is the whole reason repo_write is enforced at the
	// mount as well: this list binds a delegate that has no shell.
	"write_file": PermRepoWrite,
	"edit_file":  PermRepoWrite,
	"git_commit": PermRepoWrite,
	"git_push":   PermRepoWrite,
	"git_branch": PermRepoWrite,
	"git_pr":     PermRepoWrite,

	// Changing what aimee knows.
	"create_note":    PermKnowledgeWrite,
	"record_attempt": PermKnowledgeWrite,
}

// ToolRequiresPermission names the permission a tool needs, or "" when it needs
// none beyond `tools`.
func ToolRequiresPermission(tool string) string {
	return toolPermissions[tool]
}

// DeniedTools lists every tool this permission set withholds, sorted.
//
// Returned with the resolved set so the caller can filter and refuse from one
// answer. A delegate holding no `tools` at all is not special-cased here: it is
// given no tools by a decision made earlier, and returning "everything" would
// tell a confusing story in the logs.
func DeniedTools(permissions Permissions) []string {
	denied := make([]string, 0, len(toolPermissions))
	for tool, needed := range toolPermissions {
		if !permissions.Has(needed) {
			denied = append(denied, tool)
		}
	}
	sort.Strings(denied)
	return denied
}
