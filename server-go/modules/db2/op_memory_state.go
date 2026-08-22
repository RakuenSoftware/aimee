package db2

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageLifecycleGetState,
		db2contract.OperationLifecycleGetState, lifecycleGetState)
	Register(db2contract.StageMemoryFirstEpisodeCard,
		db2contract.OperationMemoryFirstEpisodeCard, memoryFirstEpisodeCard)
	Register(db2contract.StageMemoryRetroScanMarker,
		db2contract.OperationMemoryRetroScanMarker, memoryRetroScanMarker)
	Register(db2contract.StageDedupeByKey,
		db2contract.OperationDedupeByKey, dedupeByKey)
	Register(db2contract.StageEnrollmentTouchLastSeen,
		db2contract.OperationEnrollmentTouchLastSeen, enrollmentTouchLastSeen)
}

const lifecycleGetStateQuery = `SELECT lifecycle_state FROM memories WHERE id = $1`

// lifecycleGetState reads one memory's lifecycle state, empty when absent.
func lifecycleGetState(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	memoryID, err := db2contract.DecodeLifecycleGetStateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	state, status := readOptionalText(ctx, store, lifecycleGetStateQuery, int64(memoryID))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeLifecycleGetStateReply(state)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryFirstEpisodeCardQuery = `SELECT id FROM memory_units
 WHERE memory_id = $1 AND unit_type = 'episode_card' LIMIT 1`

// memoryFirstEpisodeCard reads the identifier of a memory's episode card.
//
// LIMIT 1 with no ORDER BY, so "first" means whichever the database offers when
// a memory somehow has several. That is the C behaviour and the shape is only
// sensible because a memory is expected to have at most one.
func memoryFirstEpisodeCard(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeMemoryFirstEpisodeCardRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	cardID, status := readOptionalInt(ctx, store, memoryFirstEpisodeCardQuery, int64(memoryID))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeMemoryFirstEpisodeCardReply(clampToU64(cardID))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryRetroScanMarkerQuery = `INSERT INTO contradiction_log
 (detected_at, memory_a_id, memory_b_id, resolution, details)
 VALUES ($1, NULL, NULL, 'scan', 'retroactive_scan')`

// memoryRetroScanMarker records that a retroactive contradiction scan ran.
//
// A row in contradiction_log naming no memories on either side: it is a marker
// in the same ledger the scan writes its findings to, so "when did this last
// run" is answerable from one table. Both memory ids are NULL, which is what
// distinguishes a marker from a finding.
func memoryRetroScanMarker(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	timestamp, err := db2contract.DecodeMemoryRetroScanMarkerRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryRetroScanMarkerQuery, timestamp)
	return acknowledgement(execErr == nil, db2contract.EncodeMemoryRetroScanMarkerReply)
}

// The duplicate set, expressed once. DISTINCT ON picks the canonical row per
// key -- the same first-row-wins rule the C gets from walking a result ordered
// by key then confidence -- and everything else sharing that key is a duplicate.
//
// One difference from the C, and it is a correction. The C compares keys after
// copying them into a 512-byte buffer, so two keys that differ only past 512
// bytes are one key to it and get merged. Here they are two keys and are not.
// Merging two genuinely different memories is the worse error of the pair.
const dedupeDuplicatesCTE = `WITH canonical AS (
 SELECT DISTINCT ON (key) key, id FROM memories
 WHERE merged_into = 0 AND tier NOT IN ('L0')
 ORDER BY key, confidence DESC),
 duplicates AS (
 SELECT m.id, canonical.id AS canonical_id FROM memories m
 JOIN canonical ON canonical.key = m.key
 WHERE m.merged_into = 0 AND m.tier NOT IN ('L0') AND m.id <> canonical.id)`

const dedupeCountQuery = dedupeDuplicatesCTE + `
 SELECT COUNT(*) FROM duplicates`

// dedupeMergeQuery merges and records in one statement.
//
// The C loops: one UPDATE per duplicate, then a best-effort provenance insert
// after each. Its own comment explains why the record matters -- the merge is
// applied autonomously, nothing gates it and no human reviews it, so a row that
// acquires merged_into with no trace of when or into which canonical makes an
// incorrect merge both unnoticeable and un-undoable.
//
// That comment also calls the record best-effort, on the grounds that an audit
// write must never fail the maintenance pass. The two halves disagree, and this
// resolves them the other way: a merge and its record land together or neither
// does. A merge nobody can find is precisely the failure the first half is
// about, and the loop has a second problem the single statement removes -- it
// is not atomic, so an interrupted pass leaves some rows merged and no way to
// tell which pass did it.
//
// The timestamp is ISO-8601 with a Z, which is what the C provenance writer
// produces and is not the format the rest of this schema uses.
const dedupeMergeQuery = dedupeDuplicatesCTE + `,
 merged AS (
 UPDATE memories SET merged_into = duplicates.canonical_id FROM duplicates
 WHERE memories.id = duplicates.id
 RETURNING memories.id, duplicates.canonical_id)
 INSERT INTO memory_provenance (memory_id, session_id, action, details, created_at)
 SELECT merged.id, '', 'dedupe_merge',
 'merged_into=' || merged.canonical_id || ' (duplicate key, auto)',
 to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
 FROM merged`

// dedupeByKey merges memories that share a key into the most confident of them.
//
// A dry run counts the same set without touching it, so an operator can see
// what a pass would do before letting it.
func dedupeByKey(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	dryRun, err := db2contract.DecodeDedupeByKeyRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var merged int64
	if dryRun != 0 {
		count, status := readOptionalInt(ctx, store, dedupeCountQuery)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		merged = count
	} else {
		// The insert's row count is the merge count: every merged row gets
		// exactly one provenance record, which is the invariant the statement
		// exists to hold.
		recorded, execErr := store.Exec(ctx, dedupeMergeQuery)
		if execErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		merged = recorded
	}

	reply, err := db2contract.EncodeDedupeByKeyReply(clampToU32(merged))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const enrollmentTouchQuery = `INSERT INTO kb_enrollments
 (scope, fingerprint, legacy, last_seen_at, authority_id)
 VALUES ($1, $2, 1, to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'), $3)
 ON CONFLICT (fingerprint) DO UPDATE
 SET last_seen_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')`

// enrollmentTouchLastSeen records that a certificate was used.
//
// The conflict path touches only last_seen_at -- never state, never revoked_at
// -- so using a revoked certificate does not resurrect it. That is the whole
// security property of this statement and the reason it is not a plain upsert
// of every column.
//
// The insert path backfills a legacy row for a certificate enrolled before the
// scheme carried one, which is why `legacy` is 1 and an authority id is minted
// here. On the conflict path the minted id is discarded, so it is generated
// unconditionally rather than lazily -- the same as the C.
func enrollmentTouchLastSeen(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	fingerprint, scope, err := db2contract.DecodeEnrollmentTouchLastSeenRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if fingerprint == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if !enrollmentDebounce.shouldWrite(fingerprint, time.Now()) {
		// Nothing was written and that is the expected answer: the reply says
		// the sighting is recorded, and a sighting inside the window already is.
		return acknowledgement(true, db2contract.EncodeEnrollmentTouchLastSeenReply)
	}
	authorityID, err := newAuthorityID()
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	_, execErr := store.Exec(ctx, enrollmentTouchQuery, scope, fingerprint, authorityID)
	return acknowledgement(execErr == nil, db2contract.EncodeEnrollmentTouchLastSeenReply)
}

// newAuthorityID mints the 128-bit identifier a backfilled enrollment carries.
func newAuthorityID() (string, error) {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw), nil
}

// Every request on a mutually-authenticated connection touches an enrollment,
// so without a debounce this is a write per request for a column nothing reads
// at that resolution. The window and the table size are the C's.
const (
	enrollmentDebounceWindow  = 5 * time.Minute
	enrollmentDebounceEntries = 256
)

var enrollmentDebounce = &fingerprintDebounce{seen: map[string]time.Time{}}

// fingerprintDebounce remembers recent sightings.
//
// Per-process, like the C's, which means several processes each write once per
// window rather than one process writing once. That was already true of the C
// and is a property of debouncing in the caller rather than the database.
type fingerprintDebounce struct {
	mu   sync.Mutex
	seen map[string]time.Time
}

func (d *fingerprintDebounce) shouldWrite(fingerprint string, now time.Time) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if at, ok := d.seen[fingerprint]; ok && now.Sub(at) < enrollmentDebounceWindow {
		return false
	}
	// Bounded, and it evicts what has aged out before it evicts anything else.
	// A full table of live entries drops the oldest, which is the C's behaviour
	// when every slot is occupied.
	if len(d.seen) >= enrollmentDebounceEntries {
		oldestFingerprint, oldestAt := "", now
		for candidate, at := range d.seen {
			if now.Sub(at) >= enrollmentDebounceWindow {
				delete(d.seen, candidate)
				continue
			}
			if oldestFingerprint == "" || at.Before(oldestAt) {
				oldestFingerprint, oldestAt = candidate, at
			}
		}
		if len(d.seen) >= enrollmentDebounceEntries && oldestFingerprint != "" {
			delete(d.seen, oldestFingerprint)
		}
	}
	d.seen[fingerprint] = now
	return true
}
