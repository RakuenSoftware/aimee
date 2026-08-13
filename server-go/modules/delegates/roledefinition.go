package delegates

import (
	"fmt"
	"strings"
)

// Reading a role an operator wrote.
//
// A role template carries its permissions in the same leading frontmatter block
// that already carries max_turns:
//
//	---
//	max_turns: 40
//	permissions:
//	  - tools
//	  - name: repo_write
//	    scopes: [/srv/repo-a, /srv/repo-b]
//	    enforced_at: mount
//	---
//
// The text is parsed HERE, not by the caller. What a permission block means is
// a rule, and a rule read in two places is a rule that can be read two ways.
// The caller's job is to hand over the bytes it found on disk.
//
// This reads the subset above and nothing else. It is not a YAML parser and
// does not pretend to be: anchors, block scalars, nested maps and flow maps are
// not supported, and a line it cannot read is an error rather than a silent
// omission. A permission an operator wrote and this dropped would be a power
// they believe they granted and did not.

const (
	roleDefinitionMaxGrants = 64
	roleDefinitionMaxScopes = 64
)

// ParseRoleDefinition reads a permissions block out of role template
// frontmatter. It returns nil when the text carries no `permissions:` key at
// all, which means "this role was not defined" and is NOT the same as a
// definition granting nothing.
func ParseRoleDefinition(text string) (*RoleDefinition, error) {
	lines := frontmatterLines(text)
	start := -1
	for i, line := range lines {
		if strings.TrimSpace(line) == "permissions:" && !isIndented(line) {
			start = i + 1
			break
		}
	}
	if start < 0 {
		return nil, nil
	}

	definition := &RoleDefinition{}
	var current *Grant
	for _, raw := range lines[start:] {
		line := strings.TrimRight(raw, " \t")
		if strings.TrimSpace(line) == "" {
			continue
		}
		if !isIndented(line) {
			break // the block ended and another key began
		}
		body := strings.TrimSpace(line)

		if item, ok := strings.CutPrefix(body, "- "); ok {
			if len(definition.Grants) >= roleDefinitionMaxGrants {
				return nil, fmt.Errorf("more than %d permissions", roleDefinitionMaxGrants)
			}
			definition.Grants = append(definition.Grants, Grant{})
			current = &definition.Grants[len(definition.Grants)-1]
			body = strings.TrimSpace(item)
			if !strings.Contains(body, ":") {
				current.Name = body // the bare form: a name on its own
				continue
			}
		}
		if current == nil {
			return nil, fmt.Errorf("permission field outside any permission: %q", body)
		}

		key, value, ok := strings.Cut(body, ":")
		if !ok {
			return nil, fmt.Errorf("permission line is neither a name nor a field: %q", body)
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		switch key {
		case "name":
			current.Name = unquote(value)
		case "enforced_at":
			current.EnforcedAt = EnforcementPoint(unquote(value))
		case "scopes":
			scopes, err := parseScopeList(value)
			if err != nil {
				return nil, err
			}
			current.Scopes = scopes
		default:
			return nil, fmt.Errorf("unknown permission field %q", key)
		}
	}

	seen := map[string]bool{}
	for _, grant := range definition.Grants {
		if grant.Name == "" {
			return nil, fmt.Errorf("a permission has no name")
		}
		// A permission listed twice is a contradiction, and resolving it here
		// would mean choosing for the operator. The old merge chose the
		// permissive reading -- an unscoped mention beat a scoped one -- so
		// repeating `repo_write` after scoping it granted every repository.
		// Say which name, and let them decide which line they meant.
		if seen[normalisePermission(grant.Name)] {
			return nil, fmt.Errorf("%q is listed twice: say it once, with the scopes you mean",
				grant.Name)
		}
		seen[normalisePermission(grant.Name)] = true
		// A scope on a built-in that nothing narrows by object would read as a
		// restriction the delegate does not actually have. Refuse it here, where
		// the operator is looking, rather than carrying it to no effect.
		if len(grant.Scopes) > 0 && DefaultEnforcementPoint(grant.Name) != EnforceNone &&
			!scopeIsEvaluated(grant.Name) {
			return nil, fmt.Errorf(
				"%q cannot be scoped: nothing evaluates a scope for it, so the scope would "+
					"narrow nothing. Of the built-in permissions only %v can be scoped",
				grant.Name, ScopableBuiltins())
		}
	}
	return definition, nil
}

// frontmatterLines returns the lines inside a leading --- block, or every line
// when there is no such block. A caller that hands over a whole template still
// gets its frontmatter read; one that hands over just the block still works.
func frontmatterLines(text string) []string {
	lines := strings.Split(text, "\n")
	if len(lines) == 0 || strings.TrimSpace(lines[0]) != "---" {
		return lines
	}
	for i := 1; i < len(lines); i++ {
		if strings.TrimSpace(lines[i]) == "---" {
			return lines[1:i]
		}
	}
	return lines[1:]
}

func isIndented(line string) bool {
	return len(line) > 0 && (line[0] == ' ' || line[0] == '\t')
}

func unquote(value string) string {
	if len(value) >= 2 {
		if (value[0] == '"' && value[len(value)-1] == '"') ||
			(value[0] == '\'' && value[len(value)-1] == '\'') {
			return value[1 : len(value)-1]
		}
	}
	return value
}

// parseScopeList reads the flow form: [a, b].
//
// An empty list is refused. A permission scoped to nothing is a permission that
// was not granted, and the wire cannot tell an empty list from an unscoped
// grant -- so accepting it would mean the answer depended on whether the module
// was asked in-process or over the bus. Omit the permission instead.
func parseScopeList(value string) ([]string, error) {
	if !strings.HasPrefix(value, "[") || !strings.HasSuffix(value, "]") {
		return nil, fmt.Errorf("scopes must be written as [a, b], got %q", value)
	}
	inner := strings.TrimSpace(value[1 : len(value)-1])
	if inner == "" {
		return nil, fmt.Errorf("scopes is empty: a permission scoped to nothing was not granted, " +
			"so leave it out instead")
	}
	var scopes []string
	for _, part := range strings.Split(inner, ",") {
		scope := unquote(strings.TrimSpace(part))
		if scope == "" {
			return nil, fmt.Errorf("empty scope in %q", value)
		}
		if len(scopes) >= roleDefinitionMaxScopes {
			return nil, fmt.Errorf("more than %d scopes", roleDefinitionMaxScopes)
		}
		scopes = append(scopes, scope)
	}
	return scopes, nil
}
