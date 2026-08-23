package db2

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"log"
	"sort"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageCodeProjectUpsert,
		db2contract.OperationCodeProjectUpsert, codeProjectUpsert)
	Register(db2contract.StageProjectionSyncProject,
		db2contract.OperationProjectionSyncProject, projectionSyncProject)
}

// The project row, locked, because everything that follows decides what to do
// from what it says.
//
// The C locks it too, except under its SQLite shim, where it cannot. The shim
// is not carried: this store is Postgres and nothing else.
const codeProjectReadQuery = `SELECT id, root, lifecycle_state, current_generation
 FROM projects WHERE name = $1 FOR UPDATE`

const codeProjectInsertQuery = `INSERT INTO projects
 (name, root, scanned_at, lifecycle_state, current_generation)
 VALUES ($1, $2, pg_now_text(), 'current', 1) RETURNING id`

// Reattaching a detached project supersedes what it had. A checkout that merely
// moved does not: the alias and the generation root change, but the indexed
// contents did not, and a new generation would say they had.
const codeProjectSupersedeQuery = `UPDATE code_project_generations
 SET state = 'superseded', detached_at = pg_now_text()
 WHERE project_id = $1 AND state = 'current'`

const codeProjectRetireAliasesQuery = `UPDATE code_project_aliases
 SET is_current = 0 WHERE project_id = $1 AND is_current = 1`

const codeProjectUpdateQuery = `UPDATE projects
 SET root = $1, scanned_at = pg_now_text(), lifecycle_state = 'current',
     current_generation = $2
 WHERE id = $3`

const codeProjectGenerationQuery = `INSERT INTO code_project_generations
 (project_id, generation, root, state, created_at, detached_at)
 VALUES ($1, $2, $3, 'current', pg_now_text(), '')
 ON CONFLICT (project_id, generation) DO UPDATE
 SET root = EXCLUDED.root, state = 'current', detached_at = ''`

const codeProjectAliasOwnerQuery = `SELECT project_id
 FROM code_project_aliases WHERE alias = $1`

const codeProjectAliasQuery = `INSERT INTO code_project_aliases
 (project_id, alias, alias_kind, is_current, first_seen_at, last_seen_at)
 VALUES ($1, $2, 'checkout', 1, pg_now_text(), pg_now_text())
 ON CONFLICT (alias) DO UPDATE
 SET project_id = EXCLUDED.project_id, is_current = 1,
     last_seen_at = EXCLUDED.last_seen_at`

// codeProjectUpsert names a project and the checkout it currently lives in, and
// answers the identifier everything else in the index hangs off.
//
// One transaction, because the halves must not be separately visible: a project
// row pointing at a generation that does not exist yet, or an alias claiming a
// checkout for a project whose row was never written, is worse than no row.
func codeProjectUpsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, projectRoot, err :=
		db2contract.DecodeCodeProjectUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var projectID int64
	txErr := store.InTx(ctx, func(tx Store) error {
		var storedRoot, lifecycleState string
		var generation int64
		scanErr := tx.QueryRow(ctx, codeProjectReadQuery, project).Scan(
			&projectID, &storedRoot, &lifecycleState, &generation)
		if scanErr == pgx.ErrNoRows {
			// A project nothing has seen before: one row, generation one, and
			// then the same generation and alias records every other path
			// ends with.
			if insertErr := tx.QueryRow(ctx, codeProjectInsertQuery, project,
				projectRoot).Scan(&projectID); insertErr != nil {
				return insertErr
			}
			return recordProjectGeneration(ctx, tx, projectID, 1, project,
				projectRoot)
		}
		if scanErr != nil {
			return scanErr
		}
		if generation < 1 {
			// A generation below one is not a generation. The C floors it for
			// the same reason the column's own CHECK does.
			generation = 1
		}
		reattaching := lifecycleState != "current"
		if reattaching {
			if _, execErr := tx.Exec(ctx, codeProjectSupersedeQuery,
				projectID); execErr != nil {
				return execErr
			}
			generation++
		}
		if storedRoot != projectRoot || reattaching {
			if _, execErr := tx.Exec(ctx, codeProjectRetireAliasesQuery,
				projectID); execErr != nil {
				return execErr
			}
		}
		_, execErr := tx.Exec(ctx, codeProjectUpdateQuery, projectRoot,
			generation, projectID)
		if execErr != nil {
			return execErr
		}
		return recordProjectGeneration(ctx, tx, projectID, generation, project,
			projectRoot)
	})
	if txErr != nil || projectID <= 0 {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCodeProjectUpsertReply(
		uint64(projectID))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// recordProjectGeneration writes the generation row and, when the root is a
// real path, claims it as this project's checkout.
//
// Only concrete paths become aliases. A label like "remote" is deliberately not
// unique and must never collide with another project's.
func recordProjectGeneration(ctx context.Context, tx Store, projectID,
	generation int64, project, projectRoot string) error {
	if _, err := tx.Exec(ctx, codeProjectGenerationQuery, projectID, generation,
		projectRoot); err != nil {
		return err
	}
	if !strings.HasPrefix(projectRoot, "/") {
		return nil
	}
	// A checkout another project already claims is a re-index under a new
	// name, not an error -- the C says so at length, having previously
	// rejected it and made a directory scannable exactly once, ever. The
	// alias answers who owns this checkout now, and it moves. It is said out
	// loud because a silent transfer would be as bad as the silent refusal.
	var aliasOwner int64
	switch err := tx.QueryRow(ctx, codeProjectAliasOwnerQuery, projectRoot).
		Scan(&aliasOwner); {
	case err == pgx.ErrNoRows:
	case err != nil:
		return err
	case aliasOwner != projectID:
		log.Printf("db2: checkout %q reindexed under project %q (was project id %d)",
			projectRoot, project, aliasOwner)
	}
	_, err := tx.Exec(ctx, codeProjectAliasQuery, projectID, projectRoot)
	return err
}

// graphEndpointMax is the C's GRAPH_ENDPOINT_MAX: the width of every buffer a
// node key is built in, and therefore the length past which a key is replaced
// by a hash of itself.
const graphEndpointMax = 512

// The C's node kinds and relation kinds, from memory_ontology.h. Only the ones
// the projection emits are named -- the rest are integers this operation never
// writes, and a table of them would be a claim that it might.
const (
	nodeKindFile     = 0
	nodeKindFunction = 1
	nodeKindStruct   = 2
	nodeKindModule   = 3
	nodeKindOther    = 99

	relationDependsOn  = 0
	relationImplements = 1
	relationCalls      = 5
	relationOther      = 99
)

// structuralWeightForRelation is the C's table: how much a relation says about
// structure, as opposed to how often it has been observed. The observed weight
// is a separate column and this never touches it.
func structuralWeightForRelation(relation string) int64 {
	switch relation {
	case "defines":
		return 3
	case "contains", "exports", "routes", "depends_on":
		return 2
	default:
		return 1
	}
}

// encodeNodeKeyComponent percent-encodes everything a key cannot carry raw.
//
// The unreserved set is the C's, "/" included: a file key keeps its path
// readable, which is most of what makes these keys greppable at all.
//
// The truncation is the C's too, and it matters more than it looks: the
// component is written into a fixed buffer and stops when the next byte would
// not fit -- and an escape is written whole or not at all, so a truncated key
// never ends in half a percent-escape.
func encodeNodeKeyComponent(value string) string {
	var encoded strings.Builder
	for index := 0; index < len(value); index++ {
		character := value[index]
		unreserved := (character >= 'A' && character <= 'Z') ||
			(character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') ||
			character == '.' || character == '_' || character == '-' ||
			character == '~' || character == '/'
		if unreserved {
			if encoded.Len() >= graphEndpointMax-1 {
				break
			}
			encoded.WriteByte(character)
			continue
		}
		if encoded.Len()+3 >= graphEndpointMax {
			break
		}
		encoded.WriteString(fmt.Sprintf("%%%02X", character))
	}
	return encoded.String()
}

// buildNodeKey is the C's key_build: prefix:encoded, or prefix:h:<hash> when
// that will not fit.
//
// The hash is over the full key rather than the component, so two keys that
// differ only in their prefix do not compact to the same thing. Half of a
// SHA-256 is enough for an identifier nothing authenticates.
func buildNodeKey(prefix, encoded string) string {
	full := prefix + ":" + encoded
	if len(full)+1 <= graphEndpointMax {
		return full
	}
	digest := sha256.Sum256([]byte(full))
	return prefix + ":h:" + hex.EncodeToString(digest[:16])
}

func projectNodeKey(project string) string {
	return buildNodeKey("project", encodeNodeKeyComponent(project))
}

func fileNodeKey(project, path string) string {
	return buildNodeKey("file", encodeNodeKeyComponent(project)+":"+
		encodeNodeKeyComponent(path))
}

func symbolNodeKey(project, name string) string {
	return buildNodeKey("symbol", encodeNodeKeyComponent(project)+":"+
		encodeNodeKeyComponent(name))
}

// prefixedNodeKey builds the export, import and route keys.
//
// These do not go through buildNodeKey in the C: they are snprintf-ed and
// dropped when they overflow, so a project with very long export names
// projects some of its exports and not others, silently. Compacting them the
// way every other key is compacted keeps them all, and keeps the one rule.
func prefixedNodeKey(prefix, project, name string) string {
	return buildNodeKey(prefix, encodeNodeKeyComponent(project)+":"+
		encodeNodeKeyComponent(name))
}

// projectionEdge is one edge the sync will write.
type projectionEdge struct {
	Source      string
	Relation    string
	Target      string
	RelationID  int64
	SubjectKind int64
	ObjectKind  int64
}

// The project, and the files in the generation it currently points at.
const projectionProjectQuery = `SELECT id FROM projects
 WHERE name = $1 AND lifecycle_state = 'current' LIMIT 1`

const projectionFilesQuery = `SELECT f.id, f.path
 FROM files f JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
 ORDER BY f.id LIMIT $2`

// projectionFileCap is the C's MAX_FILES.
const projectionFileCap = 4096

// The five per-file reads, each once for the whole project rather than once per
// file.
//
// The C issues every one of these inside its file loop, so a project of two
// thousand files runs ten thousand statements to project itself. Joined to the
// file list they are five, and they return the same rows.
const (
	projectionDefinitionsQuery = `SELECT t.file_id, t.name, t.kind
 FROM terms t JOIN files f ON f.id = t.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
   AND t.kind IN ('definition','function','method','class','struct','type','variable')
 ORDER BY t.file_id, t.name`

	projectionExportsQuery = `SELECT e.file_id, e.name
 FROM file_exports e JOIN files f ON f.id = e.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
 ORDER BY e.file_id, e.name`

	projectionImportsQuery = `SELECT i.file_id, i.name
 FROM file_imports i JOIN files f ON f.id = i.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
 ORDER BY i.file_id, i.name`

	projectionRoutesQuery = `SELECT t.file_id, t.name
 FROM terms t JOIN files f ON f.id = t.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation AND t.kind = 'route'
 ORDER BY t.file_id, t.name`

	projectionStylesQuery = `SELECT c.component_file_id, c.class_token
 FROM css_component_styles c JOIN files f ON f.id = c.component_file_id
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation AND c.class_token <> ''
 ORDER BY c.component_file_id, c.class_token`

	projectionCallsQuery = `SELECT c.file_id, c.caller, c.callee
 FROM code_calls c JOIN files f ON f.id = c.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE f.project_id = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation
   AND c.caller <> '' AND c.callee <> ''
 ORDER BY c.file_id, c.caller, c.callee`
)

// The edges, written in one statement each rather than two per edge.
//
// The observed columns are preserved by name on conflict: weight, utility and
// the utility stamp are what use has taught about an edge, and a re-projection
// of unchanged structure must not unlearn it. Everything else is structural and
// is replaced.
//
// structural_updated_at is the canonical UTC stamp where the C writes
// to_char(CURRENT_TIMESTAMP, ...), which is the session's timezone. Nothing
// reads the column yet, which is exactly why it is worth fixing now: a schema
// whose every other stamp is UTC and one column that is local is a trap for
// whoever reads it first.
const projectionEdgeUpsertQuery = `INSERT INTO entity_edges
 (source, relation, target, weight, window_id, relation_id, subject_kind,
  object_kind, edge_origin, structural_weight, structural_updated_at,
  projection_generation_id)
 SELECT edge.source, edge.relation, edge.target, 0, 0, edge.relation_id,
        edge.subject_kind, edge.object_kind, 'code_projection',
        edge.structural_weight, pg_now_text(), $8
   FROM unnest($1::text[], $2::text[], $3::text[], $4::bigint[], $5::bigint[],
               $6::bigint[], $7::bigint[])
     AS edge(source, relation, target, relation_id, subject_kind, object_kind,
             structural_weight)
 ON CONFLICT (source, relation, target) DO UPDATE SET
   relation_id = EXCLUDED.relation_id,
   subject_kind = EXCLUDED.subject_kind,
   object_kind = EXCLUDED.object_kind,
   edge_origin = EXCLUDED.edge_origin,
   structural_weight = EXCLUDED.structural_weight,
   structural_updated_at = EXCLUDED.structural_updated_at,
   projection_generation_id = EXCLUDED.projection_generation_id,
   weight = entity_edges.weight,
   utility_score = entity_edges.utility_score,
   utility_touched_at = entity_edges.utility_touched_at`

// The same write without the conflict clause, for a database whose unique index
// has not been built.
//
// Delete-then-insert rather than upsert, and both halves are in one statement
// so the edge is never briefly absent. It writes the same rows the conflict
// path would; what it cannot do is preserve the observed weight, so that is
// carried across explicitly from whatever the delete removed.
const projectionEdgeReplaceQuery = `WITH incoming AS (
   SELECT edge.source, edge.relation, edge.target, edge.relation_id,
          edge.subject_kind, edge.object_kind, edge.structural_weight
     FROM unnest($1::text[], $2::text[], $3::text[], $4::bigint[], $5::bigint[],
                 $6::bigint[], $7::bigint[])
       AS edge(source, relation, target, relation_id, subject_kind, object_kind,
               structural_weight)
 ), removed AS (
   DELETE FROM entity_edges e
    USING incoming i
    WHERE e.source = i.source AND e.relation = i.relation
      AND e.target = i.target
   RETURNING e.source, e.relation, e.target, e.weight, e.utility_score,
             e.utility_touched_at
 )
 INSERT INTO entity_edges
 (source, relation, target, weight, window_id, relation_id, subject_kind,
  object_kind, edge_origin, structural_weight, structural_updated_at,
  projection_generation_id, utility_score, utility_touched_at)
 SELECT i.source, i.relation, i.target, COALESCE(r.weight, 0), 0, i.relation_id,
        i.subject_kind, i.object_kind, 'code_projection', i.structural_weight,
        pg_now_text(), $8, COALESCE(r.utility_score, 0),
        COALESCE(r.utility_touched_at, '')
   FROM incoming i
   LEFT JOIN removed r ON r.source = i.source AND r.relation = i.relation
     AND r.target = i.target`

// The ledger row for each edge: what this generation claimed, kept even after
// the graph moves on.
const projectionEdgeRecordQuery = `INSERT INTO code_projection_edges
 (generation_id, project, source, relation, target, source_hash)
 SELECT $1, $2, source, relation, target, ''
   FROM unnest($3::text[], $4::text[], $5::text[])
     AS edge(source, relation, target)
 ON CONFLICT DO NOTHING`

const projectionCountsQuery = `UPDATE code_projection_generations
 SET edge_count = $2, node_count = $3 WHERE id = $1`

// projectionSyncProject projects a project's code index into the entity graph
// under one generation, and answers how many edges it wrote.
//
// The whole operation is one transaction. The C has none, and its failure mode
// is a generation whose ledger holds half a project: the edges it managed to
// write are indistinguishable from a project that only had those edges, and the
// counts it stamps at the end never arrive to say otherwise.
func projectionSyncProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, generationID, err :=
		db2contract.DecodeProjectionSyncProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if generationID == 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var edgeCount int
	txErr := store.InTx(ctx, func(tx Store) error {
		var projectID int64
		if scanErr := tx.QueryRow(ctx, projectionProjectQuery, project).
			Scan(&projectID); scanErr != nil {
			// Including no rows: a project nothing has indexed cannot be
			// projected, and the C says the same by returning -1.
			return scanErr
		}
		files, filesErr := readProjectionFiles(ctx, tx, projectID, project)
		if filesErr != nil {
			return filesErr
		}
		edges, edgesErr := buildProjectionEdges(ctx, tx, projectID, project, files)
		if edgesErr != nil {
			return edgesErr
		}
		edgeCount = len(edges)
		if writeErr := writeProjectionEdges(ctx, tx, int64(generationID),
			project, edges); writeErr != nil {
			return writeErr
		}
		_, countsErr := tx.Exec(ctx, projectionCountsQuery, int64(generationID),
			int64(len(edges)), int64(len(files)))
		return countsErr
	})
	if txErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeProjectionSyncProjectReply(
		uint64(edgeCount))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// projectionFile is one indexed file: its identifier, and the key every edge
// out of it is anchored on.
type projectionFile struct {
	ID  int64
	Key string
}

// readProjectionFiles reads the files this generation covers, in the C's order
// and under the C's cap, with each file's node key built once.
//
// A file whose key cannot be built is skipped by the C and skipped here. That
// cannot actually happen -- the key builder only fails on a nil argument -- but
// the ordering it implies does: a file with no key contributes no edges at all,
// not even the contains edge.
func readProjectionFiles(ctx context.Context, tx Store, projectID int64,
	project string) (map[int64]projectionFile, error) {
	rows, err := tx.Query(ctx, projectionFilesQuery, projectID,
		projectionFileCap)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	files := map[int64]projectionFile{}
	for rows.Next() {
		var fileID int64
		var path string
		if scanErr := rows.Scan(&fileID, &path); scanErr != nil {
			return nil, scanErr
		}
		files[fileID] = projectionFile{
			ID:  fileID,
			Key: fileNodeKey(project, path),
		}
	}
	return files, rows.Err()
}

// buildProjectionEdges reads the six sources of edges and turns them into the
// tuples the graph will hold.
//
// The edges are deduplicated on (source, relation, target), which the C does
// not do, and the difference shows up in the count it reports: the C counts
// upserts, so two identical call rows in one file make a generation claim two
// edges where the graph holds one. A count of edges is what the reply's name
// promises and what the generation's own edge_count is read as.
func buildProjectionEdges(ctx context.Context, tx Store, projectID int64,
	project string, files map[int64]projectionFile) ([]projectionEdge, error) {
	projectKey := projectNodeKey(project)
	collected := make([]projectionEdge, 0, len(files))
	seen := map[projectionEdge]bool{}
	add := func(edge projectionEdge) {
		if edge.Source == "" || edge.Target == "" || seen[edge] {
			return
		}
		seen[edge] = true
		collected = append(collected, edge)
	}
	// contains: the project holds its files. Emitted in the C's file order so
	// a reader diffing two generations sees the same sequence.
	for _, fileID := range sortedFileIDs(files) {
		add(projectionEdge{
			Source: projectKey, Relation: "contains",
			Target:      files[fileID].Key,
			RelationID:  relationDependsOn,
			SubjectKind: nodeKindModule, ObjectKind: nodeKindFile,
		})
	}
	// defines: a file names a symbol. The symbol's kind is narrowed where the
	// term's kind says enough to narrow it, and is otherwise left as other --
	// a definition of unknown shape is not a function.
	definitions, err := readProjectionPairs(ctx, tx, projectionDefinitionsQuery,
		projectID, true)
	if err != nil {
		return nil, err
	}
	for _, pair := range definitions {
		file, known := files[pair.FileID]
		if !known {
			continue
		}
		add(projectionEdge{
			Source: file.Key, Relation: "defines",
			Target:      symbolNodeKey(project, pair.Name),
			RelationID:  relationCalls,
			SubjectKind: nodeKindFile,
			ObjectKind:  definitionNodeKind(pair.Kind),
		})
	}
	for _, source := range []struct {
		query      string
		relation   string
		prefix     string
		relationID int64
	}{
		{projectionExportsQuery, "exports", "export", relationImplements},
		{projectionImportsQuery, "imports", "import", relationDependsOn},
		{projectionRoutesQuery, "routes", "route", relationOther},
	} {
		pairs, pairsErr := readProjectionPairs(ctx, tx, source.query, projectID,
			false)
		if pairsErr != nil {
			return nil, pairsErr
		}
		for _, pair := range pairs {
			file, known := files[pair.FileID]
			if !known {
				continue
			}
			add(projectionEdge{
				Source: file.Key, Relation: source.relation,
				Target:      prefixedNodeKey(source.prefix, project, pair.Name),
				RelationID:  source.relationID,
				SubjectKind: nodeKindFile, ObjectKind: nodeKindOther,
			})
		}
	}
	// styles: the component names a class, resolved or not. An unresolved
	// token is an edge too -- a class a component names and no stylesheet
	// defines is exactly what a reader wants to find, and dropping it would
	// make a missing style look like an absent reference.
	styles, stylesErr := readProjectionPairs(ctx, tx, projectionStylesQuery,
		projectID, false)
	if stylesErr != nil {
		return nil, stylesErr
	}
	for _, pair := range styles {
		file, known := files[pair.FileID]
		if !known {
			continue
		}
		add(projectionEdge{
			Source: file.Key, Relation: "styles",
			Target:      symbolNodeKey(project, pair.Name),
			RelationID:  relationDependsOn,
			SubjectKind: nodeKindFile, ObjectKind: nodeKindStruct,
		})
	}
	// calls: symbol to symbol. The file is how the call was found, not what
	// the edge is between, so neither endpoint is the file.
	calls, callsErr := readProjectionCalls(ctx, tx, projectID)
	if callsErr != nil {
		return nil, callsErr
	}
	for _, call := range calls {
		if _, known := files[call.FileID]; !known {
			continue
		}
		add(projectionEdge{
			Source:      symbolNodeKey(project, call.Caller),
			Relation:    "calls",
			Target:      symbolNodeKey(project, call.Callee),
			RelationID:  relationCalls,
			SubjectKind: nodeKindFunction, ObjectKind: nodeKindFunction,
		})
	}
	return collected, nil
}

// definitionNodeKind narrows a symbol by the term kind that produced it.
func definitionNodeKind(termKind string) int64 {
	switch termKind {
	case "function", "method":
		return nodeKindFunction
	case "struct", "class", "type":
		return nodeKindStruct
	default:
		return nodeKindOther
	}
}

// sortedFileIDs answers the file identifiers in the order the file read
// returned them, which is the C's order: by identifier.
func sortedFileIDs(files map[int64]projectionFile) []int64 {
	ids := make([]int64, 0, len(files))
	for id := range files {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(a, b int) bool { return ids[a] < ids[b] })
	return ids
}

// projectionPair is one (file, name) row, with the term kind when the query
// selects one.
type projectionPair struct {
	FileID int64
	Name   string
	Kind   string
}

func readProjectionPairs(ctx context.Context, tx Store, query string,
	projectID int64, withKind bool) ([]projectionPair, error) {
	rows, err := tx.Query(ctx, query, projectID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	pairs := []projectionPair{}
	for rows.Next() {
		var pair projectionPair
		var scanErr error
		if withKind {
			scanErr = rows.Scan(&pair.FileID, &pair.Name, &pair.Kind)
		} else {
			scanErr = rows.Scan(&pair.FileID, &pair.Name)
		}
		if scanErr != nil {
			return nil, scanErr
		}
		if pair.Name == "" {
			continue
		}
		pairs = append(pairs, pair)
	}
	return pairs, rows.Err()
}

// projectionCall is one caller-callee row and the file it was found in.
type projectionCall struct {
	FileID int64
	Caller string
	Callee string
}

func readProjectionCalls(ctx context.Context, tx Store, projectID int64) (
	[]projectionCall, error,
) {
	rows, err := tx.Query(ctx, projectionCallsQuery, projectID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	calls := []projectionCall{}
	for rows.Next() {
		var call projectionCall
		if scanErr := rows.Scan(&call.FileID, &call.Caller,
			&call.Callee); scanErr != nil {
			return nil, scanErr
		}
		calls = append(calls, call)
	}
	return calls, rows.Err()
}

func writeProjectionEdges(ctx context.Context, tx Store, generationID int64,
	project string, edges []projectionEdge) error {
	if len(edges) == 0 {
		return nil
	}
	sources := make([]string, len(edges))
	relations := make([]string, len(edges))
	targets := make([]string, len(edges))
	relationIDs := make([]int64, len(edges))
	subjectKinds := make([]int64, len(edges))
	objectKinds := make([]int64, len(edges))
	structuralWeights := make([]int64, len(edges))
	for index, edge := range edges {
		sources[index] = edge.Source
		relations[index] = edge.Relation
		targets[index] = edge.Target
		relationIDs[index] = edge.RelationID
		subjectKinds[index] = edge.SubjectKind
		objectKinds[index] = edge.ObjectKind
		structuralWeights[index] = structuralWeightForRelation(edge.Relation)
	}
	// The unique index the ON CONFLICT names is built by a migration rather
	// than by the schema, so whether it exists is a property of the database.
	// entity_edge_upsert probes for it and falls back; this does the same,
	// because a projection that fails wholesale on an unmigrated instance is
	// worse than one that writes its edges a slower way.
	statement := projectionEdgeUpsertQuery
	if !entityEdgeUniqueIndexReady(ctx, tx) {
		statement = projectionEdgeReplaceQuery
	}
	if _, err := tx.Exec(ctx, statement, sources, relations,
		targets, relationIDs, subjectKinds, objectKinds, structuralWeights,
		generationID); err != nil {
		return err
	}
	_, err := tx.Exec(ctx, projectionEdgeRecordQuery, generationID, project,
		sources, relations, targets)
	return err
}
