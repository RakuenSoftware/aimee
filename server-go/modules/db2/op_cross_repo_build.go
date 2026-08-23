package db2

import (
	"context"
	"errors"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCrossRepoRebuildBuildDeps,
		db2contract.OperationCrossRepoRebuildBuildDeps,
		crossRepoRebuildBuildDeps)
}

// The C's caps. A corpus at the project cap cannot map its tail repositories,
// and a manifest at the per-file cap loses its last dependencies; both are
// reported rather than absorbed.
const (
	crossRepoMaxProjects    = 4096
	crossRepoMaxDepsPerFile = 256
)

var (
	errTooManyProjects = errors.New(
		"the corpus has more projects than the build-dep mapping holds")
	errTooManyBuildDeps = errors.New(
		"a manifest declares more build dependencies than the per-file cap")
)

// Build manifests, excluding the trees a build writes into.
//
// The path exclusions are defence in depth rather than belt and braces: a
// dependency declared inside _deps/ or build/ belongs to whatever the build
// fetched, and attributing it to the repository holding the output would make
// every project depend on everything it has ever compiled.
const crossRepoBuildManifestsQuery = `SELECT p.name, f.path, fc.content
 FROM file_contents fc
 JOIN files f ON f.id = fc.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
   AND f.vendored = 0
   AND (f.path LIKE '%CMakeLists.txt' OR f.path LIKE '%.cmake'
        OR f.path LIKE '%.gitmodules' OR f.path LIKE '%Cargo.toml')
   AND f.path NOT LIKE '%/_deps/%' AND f.path NOT LIKE '%/build/%'
   AND f.path NOT LIKE '%/.git/%' AND f.path NOT LIKE '%/.aimee/%'`

const crossRepoProjectNamesQuery = `SELECT name FROM projects
 WHERE lifecycle_state = 'current' ORDER BY name LIMIT $1`

const crossRepoBuildDepInsertQuery = `INSERT INTO cross_repo_build_dep
 (caller_project, definer_project, build_kind, parse_confidence, evidence)
 SELECT dep.caller, dep.definer, dep.kind, dep.confidence, dep.evidence
   FROM unnest($1::text[], $2::text[], $3::text[], $4::text[], $5::text[])
     AS dep(caller, definer, kind, confidence, evidence)
 ON CONFLICT (caller_project, definer_project, build_kind, evidence) DO NOTHING`

// crossRepoBuildDep is one declared dependency, resolved to a project.
type crossRepoBuildDep struct {
	Caller     string
	Definer    string
	Kind       string
	Confidence string
	Evidence   string
}

// buildDepRef is one dependency reference before it has been resolved.
type buildDepRef struct {
	Ref     string
	Kind    string
	LowConf bool
}

// crossRepoRebuildBuildDeps rebuilds which project's build pulls in which
// other project.
//
// The project list is loaded first and in full: a partial list silently drops
// the dependencies pointing at the projects it did not load, which looks
// identical to those dependencies not existing.
func crossRepoRebuildBuildDeps(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCrossRepoRebuildBuildDepsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var written int64
	txErr := store.InTx(ctx, func(tx Store) error {
		byLowerName, projectsErr := readProjectNames(ctx, tx)
		if projectsErr != nil {
			return projectsErr
		}
		if _, err := tx.Exec(ctx, `DELETE FROM cross_repo_build_dep`); err != nil {
			return err
		}
		deps, readErr := readBuildDeps(ctx, tx, byLowerName)
		if readErr != nil {
			return readErr
		}
		if len(deps) == 0 {
			return nil
		}
		callers := make([]string, len(deps))
		definers := make([]string, len(deps))
		kinds := make([]string, len(deps))
		confidences := make([]string, len(deps))
		evidence := make([]string, len(deps))
		for index, dep := range deps {
			callers[index] = dep.Caller
			definers[index] = dep.Definer
			kinds[index] = dep.Kind
			confidences[index] = dep.Confidence
			evidence[index] = dep.Evidence
		}
		inserted, err := tx.Exec(ctx, crossRepoBuildDepInsertQuery, callers,
			definers, kinds, confidences, evidence)
		written = inserted
		return err
	})
	if txErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(written,
		db2contract.EncodeCrossRepoRebuildBuildDepsReply)
}

// readProjectNames maps each project's lowercased name to its real one.
//
// A lowercased name shared by two projects maps to neither: the reference being
// resolved is a repository basename, and two repositories that differ only in
// case cannot be told apart by one. The C skips the ambiguous case for the same
// reason, and an empty entry here is how that is recorded.
func readProjectNames(ctx context.Context, tx Store) (map[string]string, error) {
	rows, err := tx.Query(ctx, crossRepoProjectNamesQuery, crossRepoMaxProjects)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	byLowerName := map[string]string{}
	loaded := 0
	for rows.Next() {
		var name *string
		if scanErr := rows.Scan(&name); scanErr != nil {
			return nil, scanErr
		}
		if name == nil || *name == "" {
			continue
		}
		loaded++
		lowered := strings.ToLower(*name)
		if existing, seen := byLowerName[lowered]; seen && existing != *name {
			byLowerName[lowered] = ""
			continue
		}
		byLowerName[lowered] = *name
	}
	if rows.Err() != nil {
		return nil, rows.Err()
	}
	if loaded == crossRepoMaxProjects {
		logDroppedWrite("cross_repo_rebuild_build_deps", errTooManyProjects)
	}
	return byLowerName, nil
}

func readBuildDeps(ctx context.Context, tx Store, byLowerName map[string]string) (
	[]crossRepoBuildDep, error,
) {
	rows, err := tx.Query(ctx, crossRepoBuildManifestsQuery)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	deps := []crossRepoBuildDep{}
	for rows.Next() {
		var caller, path, content *string
		if scanErr := rows.Scan(&caller, &path, &content); scanErr != nil {
			return nil, scanErr
		}
		if caller == nil || path == nil || content == nil {
			continue
		}
		for _, ref := range extractBuildDeps(*path, *content) {
			definer := resolveBuildRef(ref.Ref, byLowerName)
			if definer == "" || definer == *caller {
				// External, ambiguous, or the project depending on itself.
				continue
			}
			confidence := "high"
			if ref.LowConf {
				confidence = "low"
			}
			deps = append(deps, crossRepoBuildDep{
				Caller: *caller, Definer: definer, Kind: ref.Kind,
				Confidence: confidence, Evidence: ref.Ref,
			})
		}
	}
	return deps, rows.Err()
}

// extractBuildDeps reads the dependency references a build manifest declares.
func extractBuildDeps(path, content string) []buildDepRef {
	base := path
	if cut := strings.LastIndexByte(base, '/'); cut >= 0 {
		base = base[cut+1:]
	}
	switch {
	case base == "CMakeLists.txt" || strings.HasSuffix(base, ".cmake"):
		return cmakeBuildDeps(content)
	case base == ".gitmodules":
		return gitmodulesBuildDeps(content)
	case base == "Cargo.toml":
		return cargoBuildDeps(content)
	}
	return nil
}

// cmakeBuildDeps finds every GIT_REPOSITORY and takes the URL after it.
//
// Comments and string literals are skipped as they are met, which is what stops
// a commented-out FetchContent block from becoming a dependency. A URL built
// from a variable or a generator expression is recorded at low confidence: the
// reference is real but its value is not known until CMake runs.
func cmakeBuildDeps(content string) []buildDepRef {
	found := []buildDepRef{}
	for offset := 0; offset < len(content); {
		switch {
		case content[offset] == '#':
			if strings.HasPrefix(content[offset:], "#[[") {
				end := strings.Index(content[offset+3:], "]]")
				if end < 0 {
					return found
				}
				offset += 3 + end + 2
				continue
			}
			newline := strings.IndexByte(content[offset:], '\n')
			if newline < 0 {
				return found
			}
			offset += newline
			continue
		case content[offset] == '"':
			offset++
			for offset < len(content) && content[offset] != '"' {
				if content[offset] == '\\' && offset+1 < len(content) {
					offset += 2
					continue
				}
				offset++
			}
			if offset < len(content) {
				offset++
			}
			continue
		}
		const keyword = "GIT_REPOSITORY"
		if !strings.EqualFold(safeSlice(content, offset, len(keyword)), keyword) {
			offset++
			continue
		}
		if offset > 0 && isCMakeIdentByte(content[offset-1]) {
			offset++
			continue
		}
		after := offset + len(keyword)
		if after < len(content) && isCMakeIdentByte(content[after]) {
			offset++
			continue
		}
		url, next := nextBuildToken(content, after)
		if url == "" {
			offset++
			continue
		}
		found = appendBuildDep(found, url, "fetchcontent",
			strings.Contains(url, "${") || strings.Contains(url, "$<"))
		offset = next
	}
	return found
}

// gitmodulesBuildDeps takes the url of every submodule.
func gitmodulesBuildDeps(content string) []buildDepRef {
	found := []buildDepRef{}
	for _, line := range strings.Split(content, "\n") {
		trimmed := strings.TrimLeft(line, " \t")
		if strings.HasPrefix(trimmed, "#") || strings.HasPrefix(trimmed, ";") {
			continue
		}
		if len(trimmed) < 3 || !strings.EqualFold(trimmed[:3], "url") {
			continue
		}
		rest := strings.TrimLeft(trimmed[3:], " \t")
		if !strings.HasPrefix(rest, "=") {
			continue
		}
		url, _ := nextBuildToken(rest, 1)
		if url == "" {
			continue
		}
		found = appendBuildDep(found, url, "submodule",
			strings.Contains(url, "${"))
	}
	return found
}

// cargoBuildDeps takes the git and path values a Cargo manifest declares.
//
// A path dependency is a dependency on a sibling checkout, which is exactly the
// cross-repository edge this table is for.
func cargoBuildDeps(content string) []buildDepRef {
	found := []buildDepRef{}
	for _, line := range strings.Split(content, "\n") {
		if comment := strings.IndexByte(line, '#'); comment >= 0 {
			line = line[:comment]
		}
		for _, key := range []string{"git", "path"} {
			for offset := 0; ; {
				relative := strings.Index(line[offset:], key)
				if relative < 0 {
					break
				}
				start := offset + relative
				offset = start + len(key)
				if start > 0 && isCMakeIdentByte(line[start-1]) {
					continue
				}
				rest := strings.TrimLeft(line[start+len(key):], " \t")
				if !strings.HasPrefix(rest, "=") {
					continue
				}
				value, _ := nextBuildToken(rest, 1)
				if value == "" {
					break
				}
				found = appendBuildDep(found, value, "manifest",
					strings.Contains(value, "${"))
				break
			}
		}
	}
	return found
}

// appendBuildDep adds a reference unless the file already declared it, and
// reports the cap rather than growing past it.
func appendBuildDep(found []buildDepRef, ref, kind string, lowConf bool) []buildDepRef {
	if ref == "" {
		return found
	}
	for _, existing := range found {
		if existing.Ref == ref && existing.Kind == kind {
			return found
		}
	}
	if len(found) >= crossRepoMaxDepsPerFile {
		logDroppedWrite("cross_repo_rebuild_build_deps", errTooManyBuildDeps)
		return found
	}
	return append(found, buildDepRef{Ref: ref, Kind: kind, LowConf: lowConf})
}

// nextBuildToken reads the next bare or quoted token, and where it ended.
func nextBuildToken(content string, offset int) (string, int) {
	for offset < len(content) && isBuildSpace(content[offset]) {
		offset++
	}
	var quote byte
	if offset < len(content) && (content[offset] == '"' || content[offset] == '\'') {
		quote = content[offset]
		offset++
	}
	var token strings.Builder
	for offset < len(content) {
		character := content[offset]
		if quote != 0 && character == quote {
			offset++
			break
		}
		if quote == 0 && (isBuildSpace(character) || character == ')') {
			break
		}
		token.WriteByte(character)
		offset++
	}
	return token.String(), offset
}

func isBuildSpace(character byte) bool {
	return character == ' ' || character == '\t' || character == '\n' ||
		character == '\r'
}

func safeSlice(content string, offset, length int) string {
	if offset+length > len(content) {
		return ""
	}
	return content[offset : offset+length]
}

// resolveBuildRef turns a dependency reference into the project it names.
//
// The reference is a URL, an scp-form git address or a path, and what matters
// in all three is the last component with any .git suffix removed. Credentials
// in a URL are dropped before the host, which also keeps a token out of the
// evidence the table would otherwise hold.
func resolveBuildRef(ref string, byLowerName map[string]string) string {
	repo := buildRefRepo(ref)
	if repo == "" {
		return ""
	}
	// An empty entry is the ambiguous case recorded by readProjectNames.
	return byLowerName[repo]
}

func buildRefRepo(ref string) string {
	if ref == "" {
		return ""
	}
	rest := ref
	at := strings.IndexByte(rest, '@')
	firstSlash := strings.IndexByte(rest, '/')
	if at >= 0 && (firstSlash < 0 || at < firstSlash) {
		rest = rest[at+1:]
	}
	if !strings.Contains(ref, "://") {
		if colon := strings.IndexByte(rest, ':'); colon >= 0 &&
			colon+1 < len(rest) {
			rest = rest[colon+1:]
		}
	}
	rest = strings.TrimRight(rest, "/\n\r \"'")
	if cut := strings.LastIndexByte(rest, '/'); cut >= 0 {
		rest = rest[cut+1:]
	}
	rest = strings.TrimSuffix(rest, ".git")
	return strings.ToLower(rest)
}
