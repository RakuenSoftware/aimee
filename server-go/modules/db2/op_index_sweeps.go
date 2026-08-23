package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEntityEdgePruneOrphans,
		db2contract.OperationEntityEdgePruneOrphans, entityEdgePruneOrphans)
	Register(db2contract.StageEntityEdgeNormalizeWeights,
		db2contract.OperationEntityEdgeNormalizeWeights,
		entityEdgeNormalizeWeights)
	Register(db2contract.StageProjectCount,
		db2contract.OperationProjectCount, projectCount)
	Register(db2contract.StagePurgeHiddenPollution,
		db2contract.OperationPurgeHiddenPollution, purgeHiddenPollution)
	Register(db2contract.StageRequeueDrifted,
		db2contract.OperationRequeueDrifted, requeueDrifted)
	Register(db2contract.StageDriftCandidates,
		db2contract.OperationDriftCandidates, driftCandidates)
	Register(db2contract.StageFileIndexDeleteProject,
		db2contract.OperationFileIndexDeleteProject, fileIndexDeleteProject)
	Register(db2contract.StageEntityObservationCount,
		db2contract.OperationEntityObservationCount, entityObservationCount)
	Register(db2contract.StageEntityProfileFresh,
		db2contract.OperationEntityProfileFresh, entityProfileFresh)
	Register(db2contract.StageCrossRepoRebuildRoutes,
		db2contract.OperationCrossRepoRebuildRoutes, crossRepoRebuildRoutes)
}

// An edge whose endpoints no longer appear in any working memory is orphaned.
//
// Projected edges are exempt, and that exemption is the important half: a code
// projection asserts edges from the index rather than from memories, so testing
// them against memory text would delete the entire projected graph.
const entityEdgePruneOrphansQuery = `DELETE FROM entity_edges e
 WHERE COALESCE(e.edge_origin, '') <> 'code_projection'
   AND NOT EXISTS (SELECT 1 FROM memories m WHERE m.tier IN ('L1','L2')
     AND (m.key LIKE '%' || e.source || '%'
          OR m.content LIKE '%' || e.source || '%'))
   AND NOT EXISTS (SELECT 1 FROM memories m WHERE m.tier IN ('L1','L2')
     AND (m.key LIKE '%' || e.target || '%'
          OR m.content LIKE '%' || e.target || '%'))`

// Weights are rescaled per relation so the strongest edge of each kind is 100.
//
// The last predicate is what keeps this idempotent: a row already at its scaled
// value is not rewritten, so running the sweep twice reports zero the second
// time rather than rewriting every row to itself.
//
// The maximum must exceed one before anything is scaled. A relation whose
// heaviest edge is one has nothing to normalise, and dividing by it would turn
// every weight into 100.
const entityEdgeNormalizeWeightsQuery = `UPDATE entity_edges
 SET weight = CAST(weight * 100.0 /
   (SELECT MAX(weight) FROM entity_edges e2
     WHERE e2.relation = entity_edges.relation) AS INTEGER)
 WHERE weight > 0
   AND COALESCE(edge_origin, '') <> 'code_projection'
   AND (SELECT MAX(weight) FROM entity_edges e2
         WHERE e2.relation = entity_edges.relation) > 1
   AND weight <> CAST(weight * 100.0 /
     (SELECT MAX(weight) FROM entity_edges e2
       WHERE e2.relation = entity_edges.relation) AS INTEGER)`

const projectCountQuery = `SELECT COUNT(*) FROM projects
 WHERE lifecycle_state = 'current'`

// A dotfile that is not a manifest is indexing noise.
//
// .gitmodules is the exception, and the exception is careful: the bare name or
// one directory deep, never a dotfile inside a dotted directory. A .git/ copy
// of it is the repository's own plumbing rather than a manifest anybody wrote.
const purgeHiddenPollutionQuery = `DELETE FROM files
 WHERE id IN (SELECT f.id FROM files f
   JOIN projects p ON p.id = f.project_id
   WHERE p.lifecycle_state = 'current'
     AND f.generation = p.current_generation
     AND (f.path LIKE '.%' OR f.path LIKE '%/.%')
     AND NOT (f.path = '.gitmodules'
              OR (f.path LIKE '%/.gitmodules' AND f.path NOT LIKE '.%'
                  AND f.path NOT LIKE '%/.%/.gitmodules')))`

// A file has drifted when its embedding no longer matches its contents.
//
// Two ways to tell, and the second is the fallback: a recorded source hash that
// disagrees, or -- for embeddings written before hashes were recorded -- a scan
// newer than the embedding. The stamp comparison normalises the separator
// because the two columns are written by different paths in different spellings.
const driftFromWhere = ` FROM code_embeddings ce
 JOIN projects p ON p.name = ce.project
 JOIN files f ON f.project_id = p.id AND f.generation = ce.generation
             AND f.path = ce.file_path
 WHERE ce.file_path <> ''
   AND p.lifecycle_state = 'current'
   AND ce.generation = p.current_generation
   AND ((ce.source_hash <> '' AND f.hash <> ce.source_hash)
        OR (ce.source_hash = ''
            AND replace(replace(f.scanned_at, 'T', ' '), 'Z', '') > ce.updated_at))`

const driftCandidatesQuery = `SELECT COUNT(*)` + driftFromWhere

// Drifted projects are queued for re-ingest, one entry each.
//
// The NOT EXISTS is per project rather than per file: a project with four
// hundred drifted files needs one re-ingest, and RETURNING counts what was
// actually inserted rather than what matched.
const requeueDriftedQuery = `INSERT INTO kb_ingest_queue
 (project, root_path, force, status)
 SELECT DISTINCT p.name, p.root, 1, 'pending'` + driftFromWhere + `
   AND NOT EXISTS (SELECT 1 FROM kb_ingest_queue q
     WHERE q.project = p.name AND q.status IN ('pending','running'))
 RETURNING project`

const fileIndexDeleteProjectQuery = `DELETE FROM kb_file_index WHERE project = $1`

// Observations are counted by distinct memory, not by row: a memory naming an
// entity five times has observed it once.
const entityObservationCountQuery = `SELECT COUNT(DISTINCT memory_id)
 FROM memory_entities WHERE LOWER(entity) = LOWER($1)`

// The window arrives as a pg_now_text modifier, so the caller decides what
// fresh means and the statement does not have to know.
const entityProfileFreshQuery = `SELECT EXISTS (SELECT 1 FROM entity_profiles
 WHERE entity_id = $1 AND last_refreshed > pg_now_text($2))`

func entityEdgePruneOrphans(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEntityEdgePruneOrphansRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	pruned, execErr := store.Exec(ctx, entityEdgePruneOrphansQuery)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(pruned, db2contract.EncodeEntityEdgePruneOrphansReply)
}

func entityEdgeNormalizeWeights(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEntityEdgeNormalizeWeightsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	normalized, execErr := store.Exec(ctx, entityEdgeNormalizeWeightsQuery)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(normalized,
		db2contract.EncodeEntityEdgeNormalizeWeightsReply)
}

func projectCount(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeProjectCountRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, projectCountQuery,
		db2contract.EncodeProjectCountReply)
}

func purgeHiddenPollution(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodePurgeHiddenPollutionRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	purged, execErr := store.Exec(ctx, purgeHiddenPollutionQuery)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(purged, db2contract.EncodePurgeHiddenPollutionReply)
}

func requeueDrifted(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeRequeueDriftedRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	queued, execErr := store.Exec(ctx, requeueDriftedQuery)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(queued, db2contract.EncodeRequeueDriftedReply)
}

// driftCandidates counts what would be re-ingested, without queueing it.
func driftCandidates(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeDriftCandidatesRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var candidates int64
	if err := store.QueryRow(ctx, driftCandidatesQuery).Scan(&candidates); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeDriftCandidatesReply(
		uint64(max(candidates, 0)))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// fileIndexDeleteProject clears a project's file index across every
// generation, unlike its current-generation neighbour.
func fileIndexDeleteProject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeFileIndexDeleteProjectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, fileIndexDeleteProjectQuery, project)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(deleted, db2contract.EncodeFileIndexDeleteProjectReply)
}

func entityObservationCount(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entityID, err := db2contract.DecodeEntityObservationCountRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return countReply(ctx, store, entityObservationCountQuery,
		db2contract.EncodeEntityObservationCountReply, entityID)
}

func entityProfileFresh(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entityID, window, err :=
		db2contract.DecodeEntityProfileFreshRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, entityProfileFreshQuery,
		db2contract.EncodeEntityProfileFreshReply, entityID, window)
}

// Routes from a caller's import specifier to the project that provides it.
//
// Restricted to module languages matched against their own identity kind, so a
// Go import cannot route to a crate that happens to share its name. A vendored
// caller file is excluded because its imports belong to the dependency, not to
// the repository holding the copy.
const crossRepoModuleRoutesQuery = `INSERT INTO cross_repo_route
 (caller_project, definer_project, kind, confidence, evidence)
 SELECT DISTINCT pc.name, ci.project, 'import_module', 'high', imp.name
 FROM file_imports imp
 JOIN files cf ON cf.id = imp.file_id
 JOIN projects pc ON pc.id = cf.project_id
 JOIN cross_repo_identity ci ON (imp.name = ci.value
   OR imp.name LIKE REPLACE(REPLACE(REPLACE(ci.value, '\', '\\'), '%', '\%'), '_', '\_') || '/%' ESCAPE '\'
   OR imp.name LIKE REPLACE(REPLACE(REPLACE(ci.value, '\', '\\'), '%', '\%'), '_', '\_') || '::%' ESCAPE '\')
 WHERE ci.project <> pc.name AND cf.vendored = 0
   AND pc.lifecycle_state = 'current'
   AND cf.generation = pc.current_generation
   AND ((cf.language = 'go' AND ci.kind = 'gomod')
        OR (cf.language = 'rust' AND ci.kind = 'crate')
        OR (cf.language IN ('js', 'ts') AND ci.kind = 'npm')
        OR (cf.language = 'python' AND ci.kind = 'pypi'))
 ON CONFLICT (caller_project, definer_project, kind, evidence) DO NOTHING`

// Routes from a C or C++ include to the project holding the header.
//
// Three filters carry the precision, and each is a finding rather than a
// preference: an include held by four or more repositories is too common to
// attribute; a bare config.h or version.h is build-generated per project and
// collides by name; and a quoted include that the caller has its own copy of
// resolves locally, so it is not a cross-repo edge at all. An angle include is
// exempt from the last one because it never resolves to the including file's
// own directory.
const crossRepoHeaderRoutesQuery = `INSERT INTO cross_repo_route
 (caller_project, definer_project, kind, confidence, evidence)
 SELECT DISTINCT pc.name, pd.name, 'import_header', 'medium', imp.name
 FROM file_imports imp
 JOIN files cf ON cf.id = imp.file_id
 JOIN projects pc ON pc.id = cf.project_id
 JOIN files fd ON (fd.path = imp.name
   OR fd.path LIKE '%/' || REPLACE(REPLACE(REPLACE(imp.name, '\', '\\'), '%', '\%'), '_', '\_') ESCAPE '\')
 JOIN projects pd ON pd.id = fd.project_id
 WHERE cf.language IN ('c', 'cpp') AND cf.vendored = 0 AND fd.vendored = 0
   AND pc.lifecycle_state = 'current' AND pd.lifecycle_state = 'current'
   AND cf.generation = pc.current_generation
   AND fd.generation = pd.current_generation
   AND pd.name <> pc.name
   AND (SELECT COUNT(DISTINCT fx.project_id) FROM files fx
        WHERE (fx.path = imp.name
               OR fx.path LIKE '%/' || REPLACE(REPLACE(REPLACE(imp.name, '\', '\\'), '%', '\%'), '_', '\_') ESCAPE '\')
          AND fx.vendored = 0
          AND fx.generation = (SELECT current_generation FROM projects px
            WHERE px.id = fx.project_id AND px.lifecycle_state = 'current')) < 4
   AND imp.name <> 'config.h' AND imp.name <> 'config.hpp'
   AND imp.name <> 'version.h' AND imp.name <> 'version.hpp'
   AND (imp.is_system = 1 OR NOT EXISTS (SELECT 1 FROM files fl
        WHERE fl.project_id = cf.project_id AND fl.vendored = 0
          AND fl.generation = pc.current_generation
          AND (fl.path = imp.name
               OR fl.path LIKE '%/' || REPLACE(REPLACE(REPLACE(imp.name, '\', '\\'), '%', '\%'), '_', '\_') ESCAPE '\')))
 ON CONFLICT (caller_project, definer_project, kind, evidence) DO NOTHING`

const crossRepoRouteCountQuery = `SELECT COUNT(*) FROM cross_repo_route`

// crossRepoRebuildRoutes rebuilds the whole route table from the index.
//
// Delete and rebuild inside one transaction, as the C does: the table is
// derived, and a reader must never see it half-built. The count is taken before
// the commit so it describes what this rebuild produced.
func crossRepoRebuildRoutes(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCrossRepoRebuildRoutesRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var routes int64
	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, `DELETE FROM cross_repo_route`); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, crossRepoModuleRoutesQuery); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, crossRepoHeaderRoutesQuery); err != nil {
			return err
		}
		return tx.QueryRow(ctx, crossRepoRouteCountQuery).Scan(&routes)
	})
	if txErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(routes, db2contract.EncodeCrossRepoRebuildRoutesReply)
}
