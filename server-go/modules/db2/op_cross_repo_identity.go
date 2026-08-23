package db2

import (
	"context"
	"errors"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCrossRepoRebuildIdentities,
		db2contract.OperationCrossRepoRebuildIdentities,
		crossRepoRebuildIdentities)
}

// crossRepoIdentityMaxPerFile is the C's CRI_MAX_PER_FILE: a manifest declaring
// more identities than this is truncated, and the truncation is logged rather
// than silently accepted.
const crossRepoIdentityMaxPerFile = 64

// errTooManyIdentities is what a truncated manifest reports. The C logs a
// warning naming the file; this reports through the same channel every
// dropped write uses.
var errTooManyIdentities = errors.New(
	"a manifest declares more identities than the per-file cap")

// Every manifest across every current project, in one read.
//
// A vendored manifest is excluded, and that is the load-bearing filter: a
// Cargo.toml under third_party/ declares the vendored library's identity, not
// the identity of the repository holding the copy. Claiming it for the host
// would route every dependent of that library to the wrong project.
const crossRepoManifestsQuery = `SELECT p.name, f.path, fc.content
 FROM file_contents fc
 JOIN files f ON f.id = fc.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
   AND f.vendored = 0
   AND (f.path LIKE '%go.mod' OR f.path LIKE '%Cargo.toml'
        OR f.path LIKE '%package.json' OR f.path LIKE '%pyproject.toml'
        OR f.path LIKE '%CMakeLists.txt' OR f.path LIKE '%.pc')`

const crossRepoIdentityInsertQuery = `INSERT INTO cross_repo_identity
 (project, kind, value)
 SELECT identity.project, identity.kind, identity.value
   FROM unnest($1::text[], $2::text[], $3::text[])
     AS identity(project, kind, value)
 ON CONFLICT (project, kind, value) DO NOTHING`

// crossRepoIdentity is one declared name and what kind of name it is.
type crossRepoIdentity struct {
	Project string
	Kind    string
	Value   string
}

// crossRepoRebuildIdentities rebuilds what each project calls itself.
//
// The whole table is derived from manifests, so it is deleted and rebuilt
// inside one transaction: a reader that saw it half-built would resolve
// dependencies against a partial set and route them to nothing.
//
// The parsing stays in Go because it is parsing -- a manifest's identity is in
// its text, and no join can read it. The reads and writes around it are one
// statement each.
func crossRepoRebuildIdentities(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCrossRepoRebuildIdentitiesRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var written int64
	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, `DELETE FROM cross_repo_identity`); err != nil {
			return err
		}
		identities, readErr := readManifestIdentities(ctx, tx)
		if readErr != nil {
			return readErr
		}
		if len(identities) == 0 {
			return nil
		}
		projects := make([]string, len(identities))
		kinds := make([]string, len(identities))
		values := make([]string, len(identities))
		for index, identity := range identities {
			projects[index] = identity.Project
			kinds[index] = identity.Kind
			values[index] = identity.Value
		}
		inserted, err := tx.Exec(ctx, crossRepoIdentityInsertQuery, projects,
			kinds, values)
		written = inserted
		return err
	})
	if txErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(written,
		db2contract.EncodeCrossRepoRebuildIdentitiesReply)
}

func readManifestIdentities(ctx context.Context, tx Store) ([]crossRepoIdentity, error) {
	rows, err := tx.Query(ctx, crossRepoManifestsQuery)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	identities := []crossRepoIdentity{}
	for rows.Next() {
		var project, path, content *string
		if scanErr := rows.Scan(&project, &path, &content); scanErr != nil {
			return nil, scanErr
		}
		if project == nil || path == nil || content == nil {
			continue
		}
		for _, found := range extractIdentities(*path, *content) {
			found.Project = *project
			identities = append(identities, found)
		}
	}
	return identities, rows.Err()
}

// The manifests that declare exactly one identity, and what kind it is.
var singleIdentityManifests = map[string]string{
	"Cargo.toml":     "crate",
	"go.mod":         "gomod",
	"package.json":   "npm",
	"pyproject.toml": "pypi",
}

// extractIdentities reads what a manifest calls its project.
//
// A .pc file is the one whose identity is its own name rather than its
// contents: pkg-config resolves "foo" by finding foo.pc, so the basename is the
// declaration.
func extractIdentities(path, content string) []crossRepoIdentity {
	base := path
	if cut := strings.LastIndexByte(base, '/'); cut >= 0 {
		base = base[cut+1:]
	}
	if kind, single := singleIdentityManifests[base]; single {
		if value := parseModuleID(base, content); value != "" {
			return []crossRepoIdentity{{Kind: kind, Value: value}}
		}
		return nil
	}
	if base == "CMakeLists.txt" {
		found := collectCMakeIdentities(content, "project", "cmake_project", nil)
		found = collectCMakeIdentities(content, "add_library", "cmake_target", found)
		return collectCMakeIdentities(content, "add_executable", "cmake_target",
			found)
	}
	if strings.HasSuffix(base, ".pc") {
		if name := strings.TrimSuffix(base, ".pc"); name != "" {
			return []crossRepoIdentity{{Kind: "pkgconfig", Value: name}}
		}
	}
	return nil
}

// parseModuleID reads the single name a package manifest declares.
func parseModuleID(base, content string) string {
	switch base {
	case "go.mod":
		return lineValue(content, "module")
	case "package.json":
		return jsonName(content)
	case "Cargo.toml", "pyproject.toml":
		// Both spell it "name" under their own section header, and the first
		// one wins -- which is the C's behaviour and is right for Cargo, where
		// [package] precedes any [dependencies] that also carry names.
		return lineValue(content, "name")
	}
	return ""
}

// lineValue reads "key value" or "key = value" from the first line that starts
// with the key.
//
// The key must be followed by a separator, so "modulepath" does not match
// "module": a manifest naming a field with the key as its prefix would
// otherwise be read as the key itself.
func lineValue(content, key string) string {
	for _, line := range strings.Split(content, "\n") {
		trimmed := strings.TrimLeft(line, " \t")
		if !strings.HasPrefix(trimmed, key) || len(trimmed) <= len(key) {
			continue
		}
		separator := trimmed[len(key)]
		if separator != ' ' && separator != '\t' && separator != '=' {
			continue
		}
		value := strings.TrimLeft(trimmed[len(key):], " \t=")
		value = strings.TrimRight(value, " \t\r")
		value = strings.Trim(value, `"'`)
		value = strings.TrimSpace(value)
		if value != "" {
			return value
		}
	}
	return ""
}

// jsonName reads the "name" field of a package.json without parsing the
// document.
//
// The C scans for the key and takes the next quoted run, and this does the
// same. A real parse would be more correct and would also accept a manifest the
// C rejects, which is a difference in what gets indexed rather than an
// improvement -- so the scan stays until both sides change together.
func jsonName(content string) string {
	key := strings.Index(content, `"name"`)
	if key < 0 {
		return ""
	}
	colon := strings.IndexByte(content[key+6:], ':')
	if colon < 0 {
		return ""
	}
	value := content[key+6+colon+1:]
	value = strings.TrimLeft(value, " \t\"")
	if end := strings.IndexByte(value, '"'); end >= 0 {
		value = value[:end]
	}
	return value
}

// collectCMakeIdentities finds every `command(name` and takes the name.
//
// Whole-token matching on the command, case-insensitively, with whitespace
// allowed before the parenthesis: `project (Foo)` and `PROJECT(Foo)` are both
// the command, and `add_subdirectory` is not `add_library` even though one
// contains the other's characters nowhere.
//
// A first argument that is a variable, a quoted string or a generator
// expression is skipped: those name something at configure time, and the
// identity table holds names that resolve without running CMake.
func collectCMakeIdentities(content, command, kind string,
	found []crossRepoIdentity) []crossRepoIdentity {
	lowered := strings.ToLower(content)
	for offset := 0; ; {
		relative := strings.Index(lowered[offset:], command)
		if relative < 0 {
			break
		}
		start := offset + relative
		offset = start + len(command)
		if start > 0 && isCMakeIdentByte(content[start-1]) {
			continue
		}
		rest := content[start+len(command):]
		if rest != "" && isCMakeIdentByte(rest[0]) {
			continue
		}
		trimmed := strings.TrimLeft(rest, " \t\n\r")
		if !strings.HasPrefix(trimmed, "(") {
			continue
		}
		if len(found) >= crossRepoIdentityMaxPerFile {
			logDroppedWrite("cross_repo_rebuild_identities",
				errTooManyIdentities)
			break
		}
		name := cmakeFirstArgument(trimmed[1:])
		if name == "" {
			continue
		}
		duplicate := false
		for _, existing := range found {
			if existing.Kind == kind && existing.Value == name {
				duplicate = true
				break
			}
		}
		if !duplicate {
			found = append(found, crossRepoIdentity{Kind: kind, Value: name})
		}
	}
	return found
}

// cmakeFirstArgument reads the identifier immediately after the parenthesis.
func cmakeFirstArgument(rest string) string {
	rest = strings.TrimLeft(rest, " \t\n\r")
	if rest == "" || rest[0] == '$' || rest[0] == '"' || rest[0] == ')' {
		return ""
	}
	end := 0
	for end < len(rest) {
		character := rest[end]
		if !isCMakeIdentByte(character) && character != '-' && character != '.' &&
			character != '+' && character != ':' {
			break
		}
		end++
	}
	return rest[:end]
}

func isCMakeIdentByte(character byte) bool {
	return (character >= 'A' && character <= 'Z') ||
		(character >= 'a' && character <= 'z') ||
		(character >= '0' && character <= '9') || character == '_'
}
