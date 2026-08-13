package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

type driftReq struct{ w wireWriter }

func newDriftReq(prompt, response, worktree string, roleIsWrite bool) *driftReq {
	r := &driftReq{}
	r.w.u32(driftRequestMagic)
	r.w.u32(uint32(wireVersion))
	var flags uint32
	if roleIsWrite {
		flags |= driftFlagWritesAllowed
	}
	r.w.u32(flags)
	r.w.str(prompt)
	r.w.str(response)
	r.w.str(worktree)
	return r
}

func (r *driftReq) paths(n int) *driftReq {
	r.w.u32(uint32(n))
	return r
}

func (r *driftReq) path(p string, exists, inDiff bool, hits ...string) *driftReq {
	r.w.str(p)
	var flags uint32
	if exists {
		flags |= driftFlagPathExist
	}
	if inDiff {
		flags |= driftFlagPathDiff
	}
	r.w.u32(flags)
	r.w.strings(hits)
	return r
}

func (r *driftReq) bytes() []byte { return r.w.buf }

func decodeDrift(t *testing.T, b []byte) (DriftSeverity, string) {
	t.Helper()
	r := &wireReader{buf: b}
	if r.u32() != driftResponseMagic {
		t.Fatalf("bad response magic")
	}
	severity := DriftSeverity(r.u32())
	message := r.str()
	if !r.done() {
		t.Fatalf("response has unread bytes")
	}
	return severity, message
}

func TestDriftStageRoundTrip(t *testing.T) {
	request := newDriftReq("implement the fix", "done", "/repo", true).
		paths(2).
		path("src/touched.c", true, true).
		path("src/never.c", false, false).
		bytes()

	response, status := handleNamedFileDrift(bus.ModuleInvocation{}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	severity, message := decodeDrift(t, response)
	if severity != DriftHard {
		t.Fatalf("want hard drift for an uncreated file, got %v (%q)", severity, message)
	}
	if message == "" {
		t.Error("a verdict must carry its wording; the caller has none of its own")
	}
}

// Index hits travel per path, and an empty list must stay distinguishable from
// hits that point elsewhere.
func TestDriftStageCarriesIndexHitsPerPath(t *testing.T) {
	request := newDriftReq("Edit stdio.h to fix the bug.", "", "", true).
		paths(1).
		path("stdio.h", false, false, "src/vendor/other.h", "src/a.c").
		bytes()

	response, status := handleNamedFileDrift(bus.ModuleInvocation{}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	if severity, message := decodeDrift(t, response); severity != DriftNone {
		t.Fatalf("hits elsewhere mean not a project file: %v (%q)", severity, message)
	}

	// The same path with NO hits is ambiguous and still needs create intent.
	request = newDriftReq("Edit stdio.h to fix the bug.", "", "", true).
		paths(1).path("stdio.h", false, false).bytes()
	response, status = handleNamedFileDrift(bus.ModuleInvocation{}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	if severity, _ := decodeDrift(t, response); severity != DriftHard {
		t.Fatalf("an empty hit list must not excuse the path, got %v", severity)
	}
}

func TestDriftStageRejectsMalformedRequests(t *testing.T) {
	good := newDriftReq("p", "", "", true).paths(1).path("a.c", true, false).bytes()

	badMagic := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], driftRequestMagic+1)

	badVersion := append([]byte(nil), good...)
	badVersion[4] = wireVersion + 1

	reservedSet := append([]byte(nil), good...)
	reservedSet[5] = 1

	unknownFlag := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(unknownFlag[8:12], driftFlagWritesAllowed|(1<<9))

	trailing := append(append([]byte(nil), good...), 0)

	truncated := good[:len(good)-1]

	hugeCount := newDriftReq("p", "", "", true).bytes()
	hugeCount = append(hugeCount, 0xff, 0xff, 0xff, 0xff)

	for name, request := range map[string][]byte{
		"bad magic":      badMagic,
		"bad version":    badVersion,
		"reserved byte":  reservedSet,
		"unknown flag":   unknownFlag,
		"trailing bytes": trailing,
		"truncated":      truncated,
		"count ceiling":  hugeCount,
		"empty":          nil,
	} {
		if _, status := handleNamedFileDrift(bus.ModuleInvocation{}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: want InvalidRequest, got %v", name, status)
		}
	}
}

// An unknown PER-PATH flag is refused too: it means the caller believes it is
// sending a fact about that file which this module is not reading.
func TestDriftStageRejectsAnUnknownPathFlag(t *testing.T) {
	r := newDriftReq("p", "", "", true)
	r.w.u32(1)
	r.w.str("a.c")
	r.w.u32(1 << 5)
	r.w.strings(nil)
	if _, status := handleNamedFileDrift(bus.ModuleInvocation{}, r.bytes()); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("want InvalidRequest, got %v", status)
	}
}

func TestDriftStageHonoursCancellation(t *testing.T) {
	request := newDriftReq("p", "", "", true).paths(0).bytes()
	if _, status := handleNamedFileDrift(bus.ModuleInvocation{DeadlineNS: 1}, request); status != bus.ModuleStatusCancelled {
		t.Errorf("want Cancelled, got %v", status)
	}
}
