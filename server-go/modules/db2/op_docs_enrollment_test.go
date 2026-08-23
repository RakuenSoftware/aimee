package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func docMetadataRow() []any {
	return []any{
		int64(4), "abc123", "guide.md", "global", "markdown", "1.2.0",
		"staged", true, "converter version changed", "2026-01-01T00:00:00Z",
		"2026-01-02T00:00:00Z",
	}
}

func TestDocReadsShareOneRowReader(t *testing.T) {
	// Eleven columns, one reader, so the single-row read and the review list
	// cannot disagree about which column is which.
	store := &fakeStore{row: &fakeRow{values: docMetadataRow()}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocReadRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, hash, filename, scope, converter, version, docState, reviewNeeded,
		reason, createdAt, updatedAt, decodeErr :=
		db2contract.DecodeKBDocReadReply(body)
	if decodeErr != nil || found != 1 || hash != "abc123" ||
		filename != "guide.md" || scope != "global" || converter != "markdown" ||
		version != "1.2.0" || docState != "staged" || reviewNeeded != 1 ||
		reason != "converter version changed" || createdAt == "" || updatedAt == "" {
		t.Fatalf("doc = %d %q %q %q %d", found, hash, filename, docState,
			reviewNeeded)
	}
}

func TestReviewQueuePagesByKey(t *testing.T) {
	// The cursor is the last identifier seen, so a document reviewed and
	// cleared between pages does not shift the window and hide the next one --
	// which an offset would.
	store := &fakeStore{rows: &fakeRows{values: [][]any{docMetadataRow()}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocListReviewRequest(16, 3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocListReview), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	waiting, decodeErr := db2contract.DecodeKBDocListReviewReply(body)
	if decodeErr != nil || len(waiting) != 1 || waiting[0].DocID != 4 ||
		waiting[0].ReviewNeeded != 1 {
		t.Fatalf("rows = %+v", waiting)
	}
	if !strings.Contains(store.lastSQL, "id > $2") ||
		!strings.Contains(store.lastSQL, "ORDER BY id ASC") {
		t.Errorf("the queue no longer pages by key: %q", store.lastSQL)
	}
	// Staged and flagged, both: a document can be flagged and already
	// published, which is a different queue.
	if !strings.Contains(store.lastSQL, "state = 'staged'") ||
		!strings.Contains(store.lastSQL, "review_needed = true") {
		t.Errorf("the queue changed shape: %q", store.lastSQL)
	}
	if store.lastArgs[1] != int64(3) {
		t.Errorf("cursor = %v", store.lastArgs[1])
	}
}

func TestAssetListIsGatedThroughTheDocument(t *testing.T) {
	// The join is what decides whether the caller may see the asset at all, and
	// four conditions do the gating.
	for _, clause := range []string{
		"d.doc_kind = 'pdf'",
		"d.quarantine_state <> 'pending'",
		"a.generation = d.generation",
		"lifecycle_state = 'current'",
	} {
		if !strings.Contains(kbDocAssetsListQuery, clause) {
			t.Errorf("missing %s", clause)
		}
	}
	// The reply carries where an asset sits and what it is, never how to fetch
	// its bytes.
	if strings.Contains(kbDocAssetsListQuery, "blob_ref") {
		t.Errorf("the blob reference is being selected: %q", kbDocAssetsListQuery)
	}
	// DISTINCT because a document is many chunks and the join is on the file
	// path, so one asset matches every chunk of its document.
	if !strings.Contains(kbDocAssetsListQuery, "SELECT DISTINCT") {
		t.Errorf("each asset would repeat per chunk: %q", kbDocAssetsListQuery)
	}
}

func TestAssetsReadInReadingOrder(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(2), 10.5, 20.5, 100.5, 30.5, "figure",
			"Figure 1: the graph", "image/png", "public"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocAssetsListRequest("aimee", "docs/a.pdf")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocAssetsList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	assets, decodeErr := db2contract.DecodeKBDocAssetsListReply(body)
	if decodeErr != nil || len(assets) != 1 || assets[0].AssetID != 4 ||
		assets[0].PageNo != 2 || assets[0].AssetKind != "figure" ||
		assets[0].SensitivityClass != "public" {
		t.Fatalf("assets = %+v", assets)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY a.page_no, a.id") {
		t.Errorf("the assets no longer read in page order: %q", store.lastSQL)
	}
}

func TestAsyncJobSaysWhoClaimedIt(t *testing.T) {
	// A job running for an hour with a worker name on it was claimed by
	// something that has since died, and nothing else in the row distinguishes
	// that from work in progress.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(900027), "embed_raw", "aimee", "running", int64(2), "",
		"worker-3", "2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z",
		"2026-01-01T00:00:00Z",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBAsyncJobGetRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBAsyncJobGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, documentID, kind, project, jobStatus, attempts, lastError,
		claimedBy, claimedAt, createdAt, updatedAt, decodeErr :=
		db2contract.DecodeKBAsyncJobGetReply(body)
	if decodeErr != nil || found != 1 || documentID != 900027 ||
		kind != "embed_raw" || project != "aimee" || jobStatus != "running" ||
		attempts != 2 || lastError != "" || claimedBy != "worker-3" ||
		claimedAt == "" || createdAt == "" || updatedAt == "" {
		t.Fatalf("job = %d %d %q %q %q", found, documentID, kind, jobStatus,
			claimedBy)
	}
}

func TestRevokingTwiceKeepsTheFirstTime(t *testing.T) {
	// The first revocation is when it happened, and a second call must not move
	// it forward -- which is what makes retrying safe.
	store := &fakeStore{row: &fakeRow{values: []any{
		"kb", "fingerprint", "01ab", "revoked", "2026-01-01T00:00:00Z",
		"2026-01-02T00:00:00Z", "2027-01-01T00:00:00Z", "2026-02-01T00:00:00Z",
		strings.Repeat("a", 32), int64(0),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentRevokeRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEnrollmentRevoke), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	revoked, scope, fingerprint, serial, state, issued, lastSeen, expires,
		revokedAt, authority, legacy, decodeErr :=
		db2contract.DecodeEnrollmentRevokeReply(body)
	if decodeErr != nil || revoked != 1 || scope != "kb" ||
		fingerprint != "fingerprint" || serial != "01ab" || state != "revoked" ||
		issued == "" || lastSeen == "" || expires == "" || revokedAt == "" ||
		authority != strings.Repeat("a", 32) || legacy != 0 {
		t.Fatalf("enrolment = %d %q %q %q", revoked, scope, state, revokedAt)
	}
	if !strings.Contains(store.lastSQL,
		"COALESCE(NULLIF(revoked_at, ''), pg_now_text())") {
		t.Errorf("a second revocation would move the time: %q", store.lastSQL)
	}
	// The reply's field is named for the normalized serial, and the resolve
	// path matches on that column -- so a caller using this reply to look the
	// enrolment up again needs the value the lookup matches on.
	if !strings.Contains(store.lastSQL, "cert_serial_norm") {
		t.Errorf("the plain serial is being returned: %q", store.lastSQL)
	}
}

func TestRevokingSomethingThatIsNotThereSaysSo(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentRevokeRequest(2147483000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEnrollmentRevoke), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	revoked, scope, _, _, state, _, _, _, _, _, _, decodeErr :=
		db2contract.DecodeEnrollmentRevokeReply(body)
	if decodeErr != nil || revoked != 0 || scope != "" || state != "" {
		t.Fatalf("revoked = %d, scope = %q, state = %q", revoked, scope, state)
	}
}

func TestTypedFactsRecallOnlyWhatIsStillTrue(t *testing.T) {
	// A superseded fact is still a row -- the table keeps its history -- and
	// recalling it would answer with something the graph has since replaced.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "aimee", "system", "depends_on", "postgres", "system",
			int64(90), "scan", "2026-01-01T00:00:00Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTypedFactRecallRequest("aimee", "", 16)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTypedFactRecall), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	facts, decodeErr := db2contract.DecodeTypedFactRecallReply(body)
	if decodeErr != nil || len(facts) != 1 || facts[0].FactID != 4 ||
		facts[0].FactRelation != "depends_on" || facts[0].Object != "postgres" ||
		facts[0].FactConfidence != 90 {
		t.Fatalf("facts = %+v", facts)
	}
	if !strings.Contains(store.lastSQL, "active = 1") {
		t.Errorf("a superseded fact could be recalled: %q", store.lastSQL)
	}
	// Asked for one relation the identifier alone orders it; asked for all of
	// them, grouping by relation first is what makes the answer readable. One
	// statement says both.
	if !strings.Contains(store.lastSQL,
		"ORDER BY CASE WHEN $3 = '' THEN relation ELSE '' END, id") {
		t.Errorf("the two orderings collapsed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "($3 = '' OR relation = $3)") {
		t.Errorf("the relation filter changed shape: %q", store.lastSQL)
	}
}
