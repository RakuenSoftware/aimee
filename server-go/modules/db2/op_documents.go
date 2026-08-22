package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageDocumentStoredHash,
		db2contract.OperationDocumentStoredHash, documentStoredHash)
	Register(db2contract.StageDocumentChunkIds,
		db2contract.OperationDocumentChunkIds, documentChunkIDs)
	Register(db2contract.StageDocAssetsDeleteForDoc,
		db2contract.OperationDocAssetsDeleteForDoc, docAssetsDeleteForDoc)
	Register(db2contract.StagePdfTsrState,
		db2contract.OperationPdfTsrState, pdfTsrState)
	Register(db2contract.StageReleaseAddDoc,
		db2contract.OperationReleaseAddDoc, releaseAddDoc)
	Register(db2contract.StageOntologyMap,
		db2contract.OperationOntologyMap, ontologyMap)
}

// Every document read here joins projects and pins the generation, so what
// comes back is what is ingested now rather than the last thing anybody stored.
// A project that is not current answers nothing at all.
const (
	documentStoredHashQuery = `SELECT d.file_hash FROM kb_documents d
 JOIN projects p ON p.name = d.project
 WHERE d.project = $1 AND d.file_path = $2 AND p.lifecycle_state = 'current'
 AND d.generation = p.current_generation LIMIT 1`
	documentChunkIDsQuery = `SELECT d.id FROM kb_documents d
 JOIN projects p ON p.name = d.project
 WHERE d.project = $1 AND d.file_path = $2 AND p.lifecycle_state = 'current'
 AND d.generation = p.current_generation`
)

// documentStoredHash reads the content hash a file was ingested under.
//
// This is what an ingest compares against to decide whether a file has changed.
// Empty means it has never been ingested in this generation, which reads as
// "changed" and re-ingests -- the safe direction, since re-ingesting something
// unchanged costs work and skipping something changed costs correctness.
//
// LIMIT 1 because a file is chunked into many document rows that all share the
// file's hash; any of them answers.
func documentStoredHash(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, err := db2contract.DecodeDocumentStoredHashRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	hash, status := readOptionalText(ctx, store, documentStoredHashQuery, project, filePath)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeDocumentStoredHashReply(hash)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// documentChunkIDs lists the chunks a file was split into.
//
// The C materializes these before issuing any follow-up write, because libpq
// allows one active result per connection and the caller goes on to delete
// vector points and rows by id. The port has no such constraint, but the shape
// stays: the caller is handed identifiers and decides what to do with them,
// which is what lets one read serve both the purge and the invalidation.
func documentChunkIDs(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	project, filePath, err := db2contract.DecodeDocumentChunkIdsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ids, status := readIntColumn(ctx, store,
		db2contract.DocumentChunkIdsMaxRows, documentChunkIDsQuery, project, filePath)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	found := make([]db2contract.DocumentChunkIdsRow, 0, len(ids))
	for _, id := range ids {
		found = append(found, db2contract.DocumentChunkIdsRow{DocumentID: clampToU64(id)})
	}
	reply, err := db2contract.EncodeDocumentChunkIdsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The second predicate is not redundant with the first. document_key is matched
// directly, and then required to still name a PDF document in this generation:
// an asset row whose document has been removed or is no longer a PDF is left
// alone rather than deleted by a caller naming its key.
const docAssetsDeleteForDocQuery = `DELETE FROM kb_doc_assets
 WHERE project = $1 AND document_key = $2
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current')
 AND document_key IN (SELECT file_path FROM kb_documents
 WHERE project = $1 AND doc_kind = 'pdf'
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current'))`

// docAssetsDeleteForDoc removes the extracted assets belonging to one PDF.
//
// The rows go; the blobs behind them do not. A reconciliation sweep reclaims a
// blob once no row references it, so an asset deduplicated across several
// documents survives until its last referrer is gone. Deleting the blob here
// would take it out from under the others.
//
// The C reads back the deleted ids with RETURNING and counts them. The port
// takes the row count instead, which is the same number without materializing
// identifiers nobody looks at.
func docAssetsDeleteForDoc(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, documentKey, err := db2contract.DecodeDocAssetsDeleteForDocRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, docAssetsDeleteForDocQuery, project, documentKey)
	if execErr != nil {
		deleted = 0
	}
	reply, err := db2contract.EncodeDocAssetsDeleteForDocReply(clampToU32(deleted))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const pdfTsrStateQuery = `SELECT tsr_state FROM kb_documents
 WHERE project = $1 AND file_path = $2 AND doc_kind = 'pdf'
   AND quarantine_state <> 'pending'
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current') LIMIT 1`

// pdfTsrState reads how far table-structure recognition has got on a PDF.
//
// The quarantine check is an access rule, not a filter: a document awaiting
// review is withheld, so this answers empty for it exactly as the lookup does.
// Dropping the check would leak the state of a document nobody is allowed to
// read yet, which is a smaller leak than its content and the same mistake.
func pdfTsrState(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	project, documentKey, err := db2contract.DecodePdfTsrStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	state, status := readOptionalText(ctx, store, pdfTsrStateQuery, project, documentKey)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodePdfTsrStateReply(state)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const releaseAddDocQuery = `INSERT INTO release_docs (release_id, doc_id)
 VALUES ($1, $2) ON CONFLICT DO NOTHING`

// releaseAddDoc puts a document into a release.
//
// ON CONFLICT DO NOTHING, so adding the same document twice is not an error:
// assembling a release is an accumulation and a caller adding what is already
// there has nothing to correct. The reply says the statement ran rather than
// whether a row appeared, which is why the repeat is invisible to it.
func releaseAddDoc(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	releaseID, docID, err := db2contract.DecodeReleaseAddDocRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, releaseAddDocQuery, int64(releaseID), int64(docID))
	return acknowledgement(execErr == nil, db2contract.EncodeReleaseAddDocReply)
}

const (
	ontologyMapEvaluationQuery = `UPDATE ontology_evaluations
 SET status = 'mapped', mapped_to = $2,
     decided_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')
 WHERE rel_type = $1`
	relTypeMappedQuery = `UPDATE rel_types SET status = 'mapped' WHERE rel_type = $1`
	relTypeExistsQuery = `SELECT 1 FROM rel_types WHERE rel_type = $1 LIMIT 1`
)

// ontologyMap aliases a proposed relation type onto one that already exists.
//
// Three things have to hold before anything is written, and each rules out a
// different way of corrupting the ontology:
//
// The target must be a real relation type. Mapping onto a name nothing defines
// would leave every relation using the alias pointing at nothing.
//
// The two names must differ after normalization, not before. "worksFor" and
// "works_for" are the same relation, and mapping one onto the other would make
// the alias resolve to itself -- a cycle of one, which every reader following
// aliases would loop on.
//
// The evaluation must exist, matching approve and reject: deciding on a
// relation nobody proposed is the caller getting it wrong.
func ontologyMap(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	relType, target, err := db2contract.DecodeOntologyMapRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	source := normalizeRelType(relType)
	mapped := normalizeRelType(target)
	if source == "" || mapped == "" || source == mapped {
		return acknowledgement(false, db2contract.EncodeOntologyMapReply)
	}

	txErr := store.InTx(ctx, func(tx Store) error {
		present, status := rowExists(ctx, tx, relTypeExistsQuery, mapped)
		if status != bus.ModuleStatusOK {
			return errNoSuchEvaluation
		}
		if present == 0 {
			return errNoSuchEvaluation
		}
		decided, err := tx.Exec(ctx, ontologyMapEvaluationQuery, source, mapped)
		if err != nil {
			return err
		}
		if decided != 1 {
			return errNoSuchEvaluation
		}
		// Best-effort in the same sense as approve and reject: matching no row
		// is fine, because a seeded relation type may already be mapped.
		_, _ = tx.Exec(ctx, relTypeMappedQuery, source)
		return nil
	})
	return acknowledgement(txErr == nil, db2contract.EncodeOntologyMapReply)
}
