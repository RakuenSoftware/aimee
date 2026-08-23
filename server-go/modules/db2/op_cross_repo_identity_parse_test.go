package db2

import "testing"

func TestLineValueRequiresASeparatorAfterTheKey(t *testing.T) {
	// The comment: "The key must be followed by a separator, so 'modulepath'
	// does not match 'module': a manifest naming a field with the key as its
	// prefix would otherwise be read as the key itself." A module recorded as
	// the value of modulepath is not an error anywhere -- it is a project under
	// the wrong name, and every join afterwards agrees with it.
	for _, c := range []struct {
		name, content, key, want string
	}{
		{"space", "module example.invalid/a\n", "module", "example.invalid/a"},
		{"equals", "name = crate-a\n", "name", "crate-a"},
		{"tab", "module\texample.invalid/b\n", "module", "example.invalid/b"},
		{"quoted", `name = "crate-b"`, "name", "crate-b"},
		{"indented", "  name = crate-c", "name", "crate-c"},
		{"a longer key is not the key", "modulepath example.invalid/c\n", "module", ""},
		{"the key alone is not a value", "module\n", "module", ""},
		{"first match wins", "name = first\nname = second\n", "name", "first"},
	} {
		t.Run(c.name, func(t *testing.T) {
			if got := lineValue(c.content, c.key); got != c.want {
				t.Errorf("lineValue(%q, %q) = %q, want %q", c.content, c.key, got, c.want)
			}
		})
	}
}

func TestJSONNameScansRatherThanParses(t *testing.T) {
	// Deliberately a scan: "A real parse would be more correct and would also
	// accept a manifest the C rejects, which is a difference in what gets
	// indexed rather than an improvement." So these cases pin the scan's
	// behaviour, including where it is knowingly loose.
	for _, c := range []struct{ name, content, want string }{
		{"ordinary", `{"name": "pkg-a", "version": "1.0"}`, "pkg-a"},
		{"no space", `{"name":"pkg-b"}`, "pkg-b"},
		{"absent", `{"version": "1.0"}`, ""},
		{"no colon after the key", `{"name"}`, ""},
	} {
		t.Run(c.name, func(t *testing.T) {
			if got := jsonName(c.content); got != c.want {
				t.Errorf("jsonName(%q) = %q, want %q", c.content, got, c.want)
			}
		})
	}
}

func TestCMakeIdentitiesMatchWholeCommandsOnly(t *testing.T) {
	// The comment: whole-token matching, case-insensitive, whitespace allowed
	// before the parenthesis, and "add_subdirectory is not add_library".
	found := collectCMakeIdentities("project(Alpha)\nPROJECT (Beta)\n", "project",
		"cmake_project", nil)
	if len(found) != 2 || found[0].Value != "Alpha" || found[1].Value != "Beta" {
		t.Fatalf("case and spacing were not both handled: %+v", found)
	}

	// A command whose name contains another's must not be taken for it.
	if got := collectCMakeIdentities("add_subdirectory(vendor)\n", "add_library",
		"cmake_target", nil); len(got) != 0 {
		t.Errorf("add_subdirectory was read as add_library: %+v", got)
	}
	// Nor a longer identifier that ends with the command.
	if got := collectCMakeIdentities("my_project(Gamma)\n", "project",
		"cmake_project", nil); len(got) != 0 {
		t.Errorf("my_project was read as project: %+v", got)
	}

	// "A first argument that is a variable, a quoted string or a generator
	// expression is skipped: those name something at configure time, and the
	// identity table holds names that resolve without running CMake."
	for _, content := range []string{
		"project(${NAME})\n",
		`project("Quoted")` + "\n",
		"project($<CONFIG>)\n",
	} {
		if got := collectCMakeIdentities(content, "project", "cmake_project", nil); len(got) != 0 {
			t.Errorf("%q produced an identity that does not resolve without CMake: %+v",
				content, got)
		}
	}
}

func TestExtractIdentitiesReadsEachManifestAsItself(t *testing.T) {
	for _, c := range []struct {
		path, content, wantKind, wantValue string
	}{
		{"go.mod", "module example.invalid/a\n", "gomod", "example.invalid/a"},
		{"deep/go.mod", "module example.invalid/b\n", "gomod", "example.invalid/b"},
		{"package.json", `{"name": "pkg"}`, "npm", "pkg"},
		{"Cargo.toml", "[package]\nname = crate\n", "crate", "crate"},
		{"pyproject.toml", "[project]\nname = proj\n", "pypi", "proj"},
		// A .pc file is named by its FILE, not its content, which is the one
		// case where the path carries the identity.
		{"libfoo.pc", "Name: something else\n", "pkgconfig", "libfoo"},
	} {
		t.Run(c.path, func(t *testing.T) {
			got := extractIdentities(c.path, c.content)
			if len(got) != 1 {
				t.Fatalf("got %d identities %+v, want 1", len(got), got)
			}
			if got[0].Kind != c.wantKind || got[0].Value != c.wantValue {
				t.Errorf("got %s/%s, want %s/%s", got[0].Kind, got[0].Value,
					c.wantKind, c.wantValue)
			}
		})
	}

	// A manifest that names nothing produces nothing rather than an empty
	// identity, which would be a project recorded under the name "".
	if got := extractIdentities("go.mod", "// no module line\n"); got != nil {
		t.Errorf("a manifest naming nothing produced %+v", got)
	}
	if got := extractIdentities("README.md", "module example.invalid/a\n"); got != nil {
		t.Errorf("a file nothing parses produced %+v", got)
	}
}
