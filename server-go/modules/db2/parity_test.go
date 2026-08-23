package db2

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// The parity run answers the only question the unit suites and the live probes
// cannot: given the identical request bytes, does this implementation say what
// the C one says?
//
// The requests are not restated here. They come from a trace the C replay
// writes as it runs -- one line per call, carrying the request it made and the
// reply it got -- because the replay's arguments are hand-written C and a
// second copy of them in Go would drift from the first silently. Two
// implementations agreeing about calls neither of them makes is not parity.
//
// Both sides run against separately created databases seeded the same way, in
// the same order, so a divergence is the operation's and not the fixture's.
//
//	scripts/db2_parity_env.sh
//
// sets both up, runs the C side, and runs this.
const parityTraceEnvironment = "AIMEE_DB2_PARITY_TRACE"

// parityCall is one recorded call from the C replay.
type parityCall struct {
	Line      int
	EventKind uint32
	StageID   uint32
	Result    int
	Request   []byte
	Reply     []byte
}

// The C's aimee_module_call_result_t, in the order module_client.h declares it,
// mapped onto the status this module answers with.
//
// Only the results a replayed call can actually produce are mapped. Transport
// and protocol failures are the bus's, not an operation's, and a parity run
// that met one is reporting on the harness rather than on the port.
var parityResultStatus = map[int]bus.ModuleStatus{
	0: bus.ModuleStatusOK,
	1: bus.ModuleStatusCapabilityAbsent,
	3: bus.ModuleStatusCancelled,
	4: bus.ModuleStatusDeadlineExceeded,
	5: bus.ModuleStatusInvalidRequest,
	6: bus.ModuleStatusInternal,
}

// Wall-clock stamps in both of the schema's spellings.
//
// These are the one difference between the two runs that is not a difference of
// behaviour: the C replay and this run happen at different instants, so any
// reply carrying a stamp it generated differs by construction. Both spellings
// are normalised to a fixed marker before comparison, which keeps the
// comparison byte-exact everywhere else -- including the length prefix, since
// both spellings are fixed width.
var parityStampPatterns = []*regexp.Regexp{
	regexp.MustCompile(`\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z`),
	regexp.MustCompile(`\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}`),
}

// A minted identifier is the other difference that is not one of behaviour.
//
// Both implementations mint artifact and audit identifiers from random bytes,
// so a reply that echoes one back differs on every run -- against itself, not
// only across implementations. Only the UUID shape the minting produces is
// normalised, so an identifier the caller supplied and the reply echoes is
// still compared: those must match, and they do.
var parityMintedIDPattern = regexp.MustCompile(
	`[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}`)

// Not every minted identifier is UUID-shaped. An authority id is sixteen random
// bytes written as thirty-two hex characters with no dashes, and it differs
// between two runs for the same reason a UUID does.
//
// It is NOT normalised everywhere, because a thirty-two character hex run is
// also what md5() returns, and the project content fingerprint is one of those.
// That fingerprint is computed from the rows both sides wrote and must match --
// a port that computed it differently would be caught by exactly this
// comparison, and a blanket rule would blind it. So the wider normalisation is
// applied only to the operations whose reply is known to carry a minted
// identifier, and every other reply still compares its hex byte for byte.
var parityUndashedIDPattern = regexp.MustCompile(`\b[0-9a-f]{32}\b`)

// Kept to what has been shown to need it. A wider list would quietly widen
// what this test stops comparing, and nothing here would report that it had.
var parityMintsAnIdentifier = map[uint32]bool{
	db2contract.OperationEnrollmentList: true,
}

// parityNormalise removes what differs by construction: the instant a run
// happened, and the identifiers it minted while running.
func parityNormalise(body []byte) []byte {
	normalised := body
	for _, pattern := range parityStampPatterns {
		normalised = pattern.ReplaceAllFunc(normalised, func(stamp []byte) []byte {
			return []byte(strings.Repeat("T", len(stamp)))
		})
	}
	return parityMintedIDPattern.ReplaceAllFunc(normalised, func(id []byte) []byte {
		return []byte(strings.Repeat("I", len(id)))
	})
}

// parityNormaliseMintedIDs additionally masks undashed minted identifiers, for
// the operations that answer with one.
func parityNormaliseMintedIDs(body []byte) []byte {
	return parityUndashedIDPattern.ReplaceAllFunc(parityNormalise(body),
		func(id []byte) []byte {
			return []byte(strings.Repeat("I", len(id)))
		})
}

func readParityTrace(t *testing.T, path string) []parityCall {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("open trace: %v", err)
	}
	defer file.Close()
	calls := []parityCall{}
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 0, 1<<20), 1<<24)
	for line := 1; scanner.Scan(); line++ {
		fields := strings.Fields(scanner.Text())
		if len(fields) != 5 {
			t.Fatalf("trace line %d has %d fields", line, len(fields))
		}
		call := parityCall{Line: line}
		for index, target := range []*uint32{&call.EventKind, &call.StageID} {
			value, parseErr := strconv.ParseUint(fields[index], 10, 32)
			if parseErr != nil {
				t.Fatalf("trace line %d field %d: %v", line, index, parseErr)
			}
			*target = uint32(value)
		}
		result, parseErr := strconv.Atoi(fields[2])
		if parseErr != nil {
			t.Fatalf("trace line %d result: %v", line, parseErr)
		}
		call.Result = result
		call.Request = parityHexField(t, line, fields[3])
		call.Reply = parityHexField(t, line, fields[4])
		calls = append(calls, call)
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("read trace: %v", err)
	}
	return calls
}

// parityHexField decodes one hex field. A single dash is how the trace writes
// an empty body, because an empty field would collapse the line's shape.
func parityHexField(t *testing.T, line int, field string) []byte {
	t.Helper()
	if field == "-" {
		return nil
	}
	raw, err := hex.DecodeString(field)
	if err != nil {
		t.Fatalf("trace line %d: %v", line, err)
	}
	return raw
}

// parityDivergence is one disagreement, written out for triage rather than only
// printed: 504 calls produce more divergences than a test log can be read
// through, and they group into a handful of causes.
type parityDivergence struct {
	Line      int
	StageID   uint32
	Operation uint32
	Kind      string
	Detail    string
}

// A call whose C result is one of these is not compared.
//
// Cancellation, a missed deadline and a denied capability are properties of the
// call's context -- a cancel callback that fired, a deadline already past, a
// grant the caller did not hold -- and none of the three is carried in the
// request bytes. Replaying the bytes cannot reproduce them, so comparing them
// would report on the harness rather than on the port. The C replay exercises
// all three deliberately at the end of its run.
var parityContextResults = map[int]string{
	2: "capability denied",
	3: "cancelled",
	4: "deadline exceeded",
}

// Operations whose replies are expected to differ, and why.
//
// An entry here is a claim that the difference is understood and that matching
// the C would be wrong or impossible -- not that the operation is unfinished.
// Each is one of three kinds: the two implementations are reporting on
// different processes, they are holding different connections, or the C is
// wrong and copying it would carry the defect into the port.
//
// The table is checked in both directions. An accepted operation that diverges
// is not a failure; an accepted operation that does NOT diverge is, because an
// acceptance nobody can reproduce is a licence to differ that outlives its
// reason. Deleting the entry is then the fix.
var parityAcceptedDivergences = map[uint32]string{
	db2contract.OperationPoolStatus: "the pool belongs to the process that is " +
		"asked: the C reports its own libpq connection and this reports a pgxpool " +
		"with its own size and idle count. Equal numbers here would mean the " +
		"reply is not reading the pool.",
	db2contract.OperationPostgresStatus: "server-side counters read at two " +
		"different instants by two different backends -- connection counts and " +
		"transaction ids move between the runs whatever either side does.",
	db2contract.OperationKBAuditAppend: "the reply carries the row hash the " +
		"chain extended to, and the C module installs no row-hash provider, so " +
		"its chain advances with an empty hash. This computes one, which is what " +
		"makes the audit chain verifiable; matching the C would mean refusing to.",
	db2contract.OperationMiningJobTryLock: "a session advisory lock is held by " +
		"the connection that took it. The C holds one connection for the whole " +
		"run and this borrows from a pool, so the second attempt in the replay " +
		"meets its own lock there and a different connection here.",
	db2contract.OperationVectorRebuildLockTryAcquire: "the same session advisory " +
		"lock difference as mining_job_try_lock: what the reply reports is a " +
		"property of the connection, not of the operation.",
	db2contract.OperationEnrollmentAuthorityResolve: "the reply carries a hash " +
		"of the resolved authority record, which includes the stamp the row was " +
		"written with. The two runs write their rows at different instants, so " +
		"the hashes differ by construction and no normalisation can reach inside " +
		"a digest.",
	db2contract.OperationCalibrationAuditStats: "this answers with every bucket, " +
		"including the empty ones, so a caller can see which buckets have no " +
		"evidence rather than having to infer it from an absence. The C omits " +
		"them, which makes an unmeasured bucket indistinguishable from one that " +
		"was measured and found empty.",
	db2contract.OperationMiningJobGet: "the C reads mining_jobs.enabled, a " +
		"BOOLEAN column, with aimee_pg_column_int, which is atoi over the text " +
		"libpq returned -- and atoi(\"t\") is 0. So the C reports every mining " +
		"job as disabled, including the ones that are enabled and the default, " +
		"which is TRUE. This reads the column as a boolean. Copying the C would " +
		"mean answering a scheduler that nothing is ever due to run.",
	db2contract.OperationResolveContradiction: "the C acknowledges when its " +
		"update changed NO rows -- it tests the changed-row count for zero and " +
		"reports that as success, so it says a contradiction was resolved exactly " +
		"when it was not. This reports what the C meant. Copying the inversion " +
		"would carry a live defect into the port; it is recorded here and against " +
		"the C instead.",
}

func TestParityWithTheCReplay(t *testing.T) {
	tracePath := os.Getenv(parityTraceEnvironment)
	if tracePath == "" {
		t.Skip("set " + parityTraceEnvironment + " to a C replay trace to compare")
	}
	store, closeStore := liveStore(t)
	defer closeStore()
	handler := NewDispatchHandler(store)

	calls := readParityTrace(t, tracePath)
	if len(calls) == 0 {
		t.Fatal("the trace is empty; the C replay recorded nothing")
	}

	divergences := []parityDivergence{}
	acceptedSeen := map[uint32]bool{}
	compared, skipped := 0, 0
	for _, call := range calls {
		operation := uint32(0)
		if header, err := db2contract.DecodeRequestHeader(call.Request); err == nil {
			operation = header.Operation
		}
		record := func(kind, detail string) {
			divergences = append(divergences, parityDivergence{
				Line: call.Line, StageID: call.StageID, Operation: operation,
				Kind: kind, Detail: detail,
			})
		}
		if _, contextual := parityContextResults[call.Result]; contextual {
			skipped++
			continue
		}
		expectedStatus, mapped := parityResultStatus[call.Result]
		if !mapped {
			record("bus-failure", fmt.Sprintf("the C call returned %d", call.Result))
			continue
		}
		compared++
		body, status := handler(invocation(call.StageID), call.Request)
		if status != expectedStatus {
			record("status", fmt.Sprintf("go=%s c=%s",
				parityStatusName(status), parityStatusName(expectedStatus)))
			continue
		}
		if status != bus.ModuleStatusOK {
			// A refusal carries no body on either side, so the status is the
			// whole of the answer.
			continue
		}
		normalise := parityNormalise
		if parityMintsAnIdentifier[operation] {
			normalise = parityNormaliseMintedIDs
		}
		got := normalise(body)
		want := normalise(call.Reply)
		if string(got) == string(want) {
			continue
		}
		if reason, accepted := parityAcceptedDivergences[operation]; accepted {
			acceptedSeen[operation] = true
			t.Logf("line %d operation %d differs as expected: %s",
				call.Line, operation, reason)
			continue
		}
		record("reply", parityFirstDifference(got, want))
	}

	// An acceptance that never fires has outlived whatever justified it, and
	// left in place it quietly excuses a future divergence nobody decided to
	// allow.
	for operation, reason := range parityAcceptedDivergences {
		if !acceptedSeen[operation] {
			t.Errorf("operation %d is recorded as an accepted divergence but "+
				"agreed with the C: delete the entry. Its reason was: %s",
				operation, reason)
		}
	}

	t.Logf("compared %d calls, skipped %d whose result the request bytes cannot carry",
		compared, skipped)
	if reportPath := os.Getenv("AIMEE_DB2_PARITY_REPORT"); reportPath != "" {
		writeParityReport(t, reportPath, divergences)
	}
	if len(divergences) > 0 {
		for _, divergence := range divergences[:min(len(divergences), 12)] {
			t.Errorf("line %d stage %d operation %d [%s]: %s", divergence.Line,
				divergence.StageID, divergence.Operation, divergence.Kind,
				divergence.Detail)
		}
		t.Fatalf("%d of %d compared calls diverged", len(divergences), compared)
	}
	t.Logf("all %d compared calls agree", compared)
}

// writeParityReport writes one divergence per line, for a triage pass that maps
// stage and operation back to catalogue names.
func writeParityReport(t *testing.T, path string, divergences []parityDivergence) {
	t.Helper()
	var report strings.Builder
	for _, divergence := range divergences {
		fmt.Fprintf(&report, "%d\t%d\t%d\t%s\t%s\n", divergence.Line,
			divergence.StageID, divergence.Operation, divergence.Kind,
			divergence.Detail)
	}
	if err := os.WriteFile(path, []byte(report.String()), 0o644); err != nil {
		t.Fatalf("write report: %v", err)
	}
}

func parityStatusName(status bus.ModuleStatus) string {
	switch status {
	case bus.ModuleStatusOK:
		return "ok"
	case bus.ModuleStatusCapabilityAbsent:
		return "capability-absent"
	case bus.ModuleStatusCancelled:
		return "cancelled"
	case bus.ModuleStatusDeadlineExceeded:
		return "deadline-exceeded"
	case bus.ModuleStatusInvalidRequest:
		return "invalid-request"
	case bus.ModuleStatusInternal:
		return "internal"
	}
	return "status-" + itoa(uint32(status))
}

// parityFirstDifference says where two replies stop agreeing and what each has
// there, which is what distinguishes a changed field from a changed shape.
func parityFirstDifference(got, want []byte) string {
	limit := min(len(got), len(want))
	offset := 0
	for offset < limit && got[offset] == want[offset] {
		offset++
	}
	return fmt.Sprintf("len go=%d c=%d, first differ at %d: go=%s c=%s",
		len(got), len(want), offset,
		parityWindow(got, offset), parityWindow(want, offset))
}

func parityWindow(body []byte, offset int) string {
	end := min(offset+16, len(body))
	if offset >= len(body) {
		return "<end>"
	}
	return hex.EncodeToString(body[offset:end])
}
