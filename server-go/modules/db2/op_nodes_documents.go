package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEntityNodeGet,
		db2contract.OperationEntityNodeGet, entityNodeGet)
	Register(db2contract.StageEntityNodeUpsert,
		db2contract.OperationEntityNodeUpsert, entityNodeUpsert)
	Register(db2contract.StageEntityEdgeExplain,
		db2contract.OperationEntityEdgeExplain, entityEdgeExplain)
	Register(db2contract.StageKBDocRegionsForChunk,
		db2contract.OperationKBDocRegionsForChunk, kbDocRegionsForChunk)
	Register(db2contract.StageKBDocumentFetch,
		db2contract.OperationKBDocumentFetch, kbDocumentFetch)
}

// Every column moves on conflict except the key itself, which is what makes
// this a re-observation rather than a merge: a node seen again in a new
// generation is the same node, described by whatever saw it last.
//
// The generation is the load-bearing one. It is how a sweep tells a node the
// current scan found from one left behind by a scan that no longer runs.
const entityNodeUpsertQuery = `INSERT INTO entity_nodes
 (node_key, node_kind, project, display_name, full_key, file_path, symbol,
  node_origin, last_seen_generation_id, updated_at)
 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, pg_now_text())
 ON CONFLICT (node_key) DO UPDATE SET
  node_kind = excluded.node_kind,
  project = excluded.project,
  display_name = excluded.display_name,
  full_key = excluded.full_key,
  file_path = excluded.file_path,
  symbol = excluded.symbol,
  node_origin = excluded.node_origin,
  last_seen_generation_id = excluded.last_seen_generation_id,
  updated_at = pg_now_text()`

// entityNodeUpsert records that a graph node exists, or that it was seen again.
//
// The stamp is pg_now_text() where the C formats the local timestamp without a
// zone. Every other timestamp in this port is the canonical UTC spelling, and
// two spellings in one table make a comparison between rows depend on which
// writer wrote them.
func entityNodeUpsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	nodeKey, nodeKind, project, displayName, fullKey, filePath, symbol,
		origin, generation, err :=
		db2contract.DecodeEntityNodeUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, entityNodeUpsertQuery, nodeKey,
		int64(nodeKind), project, displayName, fullKey, filePath, symbol,
		origin, int64(generation))
	return acknowledgement(execErr == nil,
		db2contract.EncodeEntityNodeUpsertReply)
}

// The C selects node_key and never reads it back -- the caller supplied it --
// so it is dropped.
const entityNodeGetQuery = `SELECT node_kind, project, display_name, full_key,
 file_path, symbol, node_origin, last_seen_generation_id
 FROM entity_nodes WHERE node_key = $1`

// entityNodeGet answers what a graph node is.
//
// The origin says what put it there -- a scan, a projection, a person -- which
// is what a caller needs before deciding whether it may rewrite it.
func entityNodeGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	nodeKey, err := db2contract.DecodeEntityNodeGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var nodeKind, generation int64
	var project, displayName, fullKey, filePath, symbol, origin string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, entityNodeGetQuery, nodeKey).Scan(&nodeKind,
		&project, &displayName, &fullKey, &filePath, &symbol, &origin,
		&generation); scanErr != nil {
		found, nodeKind, generation = 0, 0, 0
		project, displayName, fullKey = "", "", ""
		filePath, symbol, origin = "", "", ""
	}
	reply, encodeErr := db2contract.EncodeEntityNodeGetReply(found,
		clampToU32(nodeKind), clampToU64(generation), project, displayName,
		fullKey, filePath, symbol, origin)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Ordered by the two weights added together, which is the whole point of the
// read: an edge that is structurally important and an edge that has been
// observed often are both worth explaining, and neither number alone ranks
// them against each other.
//
// The three COALESCEs are over columns added after the table, so rows written
// before they existed answer their neutral values rather than failing the scan.
const entityEdgeExplainQuery = `SELECT id, source, relation, target, weight,
 COALESCE(structural_weight, 0), COALESCE(utility_score, 0.0),
 COALESCE(edge_origin, '')
 FROM entity_edges
 WHERE (source = $1 OR target = $1) AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
 ORDER BY (COALESCE(structural_weight, 0) + weight) DESC
 LIMIT $2`

// entityEdgeExplain answers why an entity is connected to what it is connected
// to.
//
// Everything the ranking used, returned alongside the edge, so a caller can
// show its work: the observed weight, the structural weight, what the edge has
// proved worth, and where it came from.
func entityEdgeExplain(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, err := db2contract.DecodeEntityEdgeExplainRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EntityEdgeExplainMaxRows
	rows, queryErr := store.Query(ctx, entityEdgeExplainQuery,
		entity, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	edges := make([]db2contract.EntityEdgeExplainRow, 0, 16)
	for rows.Next() {
		var id, weight, structural int64
		var source, relation, target, origin string
		var utility float64
		if scanErr := rows.Scan(&id, &source, &relation, &target, &weight,
			&structural, &utility, &origin); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		edges = append(edges, db2contract.EntityEdgeExplainRow{
			EdgeID:           clampToU64(id),
			EdgeSource:       source,
			EdgeRelation:     relation,
			EdgeTarget:       target,
			EdgeWeight:       clampToU32(weight),
			StructuralWeight: clampToU32(structural),
			UtilityScore:     utility,
			EdgeOrigin:       origin,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeEntityEdgeExplainReply(edges)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// In line order, because the regions are where a chunk's text sits on the page
// and a caller drawing them wants them in reading order.
const kbDocRegionsForChunkQuery = `SELECT page_no, x0, y0, x1, y1, quote,
 line_index, content_type
 FROM kb_doc_regions WHERE chunk_id = $1 ORDER BY line_index LIMIT $2`

// kbDocRegionsForChunk answers where on the page a chunk came from.
//
// The rectangle and the quote together are what let a citation point at a place
// in a PDF rather than at a chunk identifier: the coordinates locate it and the
// quote proves the location is the right one.
func kbDocRegionsForChunk(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	chunkID, err := db2contract.DecodeKBDocRegionsForChunkRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.KBDocRegionsForChunkMaxRows
	rows, queryErr := store.Query(ctx, kbDocRegionsForChunkQuery,
		int64(chunkID), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	regions := make([]db2contract.KBDocRegionsForChunkRow, 0, 16)
	for rows.Next() {
		var page, lineIndex int64
		var x0, y0, x1, y1 float64
		var quote, contentType string
		if scanErr := rows.Scan(&page, &x0, &y0, &x1, &y1, &quote, &lineIndex,
			&contentType); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		regions = append(regions, db2contract.KBDocRegionsForChunkRow{
			PageNo:      clampToU32(page),
			X0:          x0,
			Y0:          y0,
			X1:          x1,
			Y1:          y1,
			RegionQuote: quote,
			LineIndex:   clampToU32(lineIndex),
			ContentType: contentType,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeKBDocRegionsForChunkReply(regions)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// An empty project fetches by identifier alone, which the C's note explains: a
// whole-corpus search returns rows from every project, and the caller resolving
// them has no project to name. A named project still scopes the lookup.
//
// The generation pinning applies either way, so a chunk from a superseded
// generation is not fetchable even by identifier -- the identifier survives a
// reindex but the row it names is no longer the current text.
const kbDocumentFetchQuery = `SELECT d.project, d.file_path, d.file_hash,
 d.heading_path, d.line_start, d.line_end, d.content, d.doc_kind
 FROM kb_documents d JOIN projects p ON p.name = d.project
 WHERE d.id = $1 AND ($2 = '' OR d.project = $2)
 AND p.lifecycle_state = 'current'
 AND d.generation = p.current_generation`

// kbDocumentFetch answers one chunk of an ingested document.
func kbDocumentFetch(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	documentID, project, err :=
		db2contract.DecodeKBDocumentFetchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var lineStart, lineEnd int64
	var docProject, filePath, fileHash, headingPath, content, docKind string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, kbDocumentFetchQuery,
		int64(documentID), project).Scan(&docProject, &filePath, &fileHash,
		&headingPath, &lineStart, &lineEnd, &content, &docKind); scanErr != nil {
		found, lineStart, lineEnd = 0, 0, 0
		docProject, filePath, fileHash = "", "", ""
		headingPath, content, docKind = "", "", ""
	}
	reply, encodeErr := db2contract.EncodeKBDocumentFetchReply(found, docProject,
		filePath, fileHash, headingPath, clampToU32(lineStart),
		clampToU32(lineEnd), content, docKind)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
