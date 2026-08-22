package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestDocumentReadsArePinnedToTheCurrentGeneration(t *testing.T) {
	// What comes back is what is ingested now, not the last thing anybody
	// stored. A project that is not current answers nothing at all.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
		rows  bool
	}{
		{
			"document_stored_hash",
			db2contract.StageDocumentStoredHash,
			func() ([]byte, error) {
				return db2contract.EncodeDocumentStoredHashRequest("replay-project", "a.md")
			},
			false,
		},
		{
			"document_chunk_ids",
			db2contract.StageDocumentChunkIds,
			func() ([]byte, error) {
				return db2contract.EncodeDocumentChunkIdsRequest("replay-project", "a.md")
			},
			true,
		},
		{
			"pdf_tsr_state",
			db2contract.StagePdfTsrState,
			func() ([]byte, error) {
				return db2contract.EncodePdfTsrStateRequest("replay-project", "a.pdf")
			},
			false,
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{}
			if testCase.rows {
				store.rows = &fakeRows{}
			}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if !strings.Contains(store.lastSQL, "current_generation") {
				t.Errorf("the read is not pinned to the current generation: %q",
					store.lastSQL)
			}
			if !strings.Contains(store.lastSQL, "lifecycle_state = 'current'") {
				t.Errorf("the read does not require a current project: %q", store.lastSQL)
			}
		})
	}
}

func TestPdfTsrStateWithholdsAQuarantinedDocument(t *testing.T) {
	// An access rule, not a filter. Dropping it would leak the state of a
	// document nobody is allowed to read yet -- a smaller leak than its
	// content and the same mistake.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePdfTsrStateRequest("replay-project", "a.pdf")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StagePdfTsrState), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "quarantine_state <> 'pending'") {
		t.Errorf("a document awaiting review would now report its state: %q", store.lastSQL)
	}
}

func TestDocAssetsDeleteRequiresThePdfToStillExist(t *testing.T) {
	// The second predicate is not redundant with the first: an asset row whose
	// document has been removed, or is no longer a PDF, is left alone rather
	// than deleted by a caller naming its key.
	store := &fakeStore{execRowsAt: true, execRows: 3}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDocAssetsDeleteForDocRequest("replay-project", "a.pdf")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDocAssetsDeleteForDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	deleted, decodeErr := db2contract.DecodeDocAssetsDeleteForDocReply(body)
	if decodeErr != nil || deleted != 3 {
		t.Fatalf("deleted = %d, want the count", deleted)
	}
	if !strings.Contains(store.lastSQL, "document_key IN (SELECT file_path FROM kb_documents") ||
		!strings.Contains(store.lastSQL, "doc_kind = 'pdf'") {
		t.Errorf("the document check is gone: %q", store.lastSQL)
	}
	// The blobs are not touched here: a deduplicated asset survives until its
	// last referrer goes, and the reconciliation sweep is what reclaims it.
	if strings.Contains(store.lastSQL, "kb_blobs") ||
		strings.Contains(store.lastSQL, "blob_ref") {
		t.Errorf("the delete reaches the blobs: %q", store.lastSQL)
	}
}

func TestReleaseAddDocIsRepeatable(t *testing.T) {
	// Assembling a release is an accumulation. A caller adding what is already
	// there has nothing to correct.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeReleaseAddDocRequest(4, 9)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageReleaseAddDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeReleaseAddDocReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d -- a row count crept into the answer", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT DO NOTHING") {
		t.Errorf("a repeat is now a constraint violation: %q", store.lastSQL)
	}
}

func TestOntologyMapRefusesToAliasARelationOntoItself(t *testing.T) {
	// Compared after normalization, not before: "worksFor" and "works_for" are
	// the same relation, and mapping one onto the other would make the alias
	// resolve to itself -- a cycle of one, which every reader following aliases
	// would loop on.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyMapRequest("worksFor", "works_for")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyMap), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeOntologyMapReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if store.txCalls != 0 {
		t.Error("a transaction was opened to map a relation onto itself")
	}
}

func TestOntologyMapRequiresTheTargetToExist(t *testing.T) {
	// Mapping onto a name nothing defines would leave every relation using the
	// alias pointing at nothing.
	store := &fakeStore{} // no rel_types row for the target
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyMapRequest("novel_relation", "no_such_target")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyMap), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeOntologyMapReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if store.execCalls != 0 {
		t.Fatalf("statements = %d -- something was written for a target that "+
			"does not exist", store.execCalls)
	}
	if !store.rolledBack {
		t.Error("the transaction was not rolled back")
	}
}

func TestOntologyMapMovesBothTablesWhenItSucceeds(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{idPtr(1)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyMapRequest("novel_relation", "works_for")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyMap), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeOntologyMapReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if store.execCalls != 2 {
		t.Fatalf("statements = %d, want the evaluation and the rel_type",
			store.execCalls)
	}
	if !strings.Contains(store.sqlLog[1], "'mapped'") ||
		!strings.Contains(store.sqlLog[1], "mapped_to = $2") {
		t.Errorf("the evaluation does not record the alias: %q", store.sqlLog[1])
	}
	if !strings.Contains(store.sqlLog[2], "rel_types") {
		t.Errorf("the relation type was not marked mapped: %q", store.sqlLog[2])
	}
}
