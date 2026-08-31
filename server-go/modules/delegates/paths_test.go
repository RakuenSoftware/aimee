package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func pathsRequest(prompt string, max int) []byte {
	request := make([]byte, pathsHeaderLen+len(prompt))
	binary.LittleEndian.PutUint32(request[0:4], pathsRequestMagic)
	request[4] = wireVersion
	request[5] = byte(max)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(prompt)))
	copy(request[pathsHeaderLen:], prompt)
	return request
}

func pathsCall(t *testing.T, prompt string) []string {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StagePaths},
		pathsRequest(prompt, pathsMax))
	if status != bus.ModuleStatusOK ||
		binary.LittleEndian.Uint32(response[0:4]) != pathsResponseMagic {
		t.Fatalf("response = %x, status = %d", response, status)
	}
	count := int(binary.LittleEndian.Uint32(response[4:8]))
	out := make([]string, 0, count)
	at := pathsRespHeaderLen
	for i := 0; i < count; i++ {
		n := int(binary.LittleEndian.Uint16(response[at : at+2]))
		at += 2
		out = append(out, string(response[at:at+n]))
		at += n
	}
	return out
}

func sameStrings(got, want []string) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range want {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

// Ported one-for-one from test_delegate_context_shed.c.
func TestExtractNamedPathsPortedCases(t *testing.T) {
	cases := []struct {
		name, prompt string
		want         []string
	}{
		{"prose names no file", "Fix the bug in the authentication module.", nil},
		{"a single named path", "Edit src/config.c to fix the parsing bug.",
			[]string{"src/config.c"}},
		{"a terminal period is punctuation, not part of the name",
			"Edit only /tmp/aimee-fixture/remaining.c. The tests in " +
				"/tmp/aimee-fixture/test_remaining.c expect a clamped result.",
			[]string{"/tmp/aimee-fixture/remaining.c", "/tmp/aimee-fixture/test_remaining.c"}},
		{"two paths", "Update src/foo.c and src/bar.h to add the new interface.",
			[]string{"src/foo.c", "src/bar.h"}},
		{"the same path twice is one target",
			"Edit src/config.c carefully; do not break src/config.c.",
			[]string{"src/config.c"}},
		// A bare filename is prose, not a repo-relative path.
		{"no separator means not a path", "Fix config.c and util.h.", nil},
	}
	for _, c := range cases {
		if got := pathsCall(t, c.prompt); !sameStrings(got, c.want) {
			t.Errorf("%s: got %v, want %v", c.name, got, c.want)
		}
	}
}

// A brief that names off-limits files must not have them read as targets, or a
// successful run reports "named file was not created" for work it was told to
// leave alone.
func TestNegatedPathsAreNotTargets(t *testing.T) {
	prompt := "Implement src/db1/agent_jobs.c with the new schema\n" +
		"Do NOT touch src/cmd_cron.c — follow-up task\n" +
		"Skip src/scheduler.c — owned by another delegate\n" +
		"Don't modify src/headers/legacy.h either\n"
	if got := pathsCall(t, prompt); !sameStrings(got, []string{"src/db1/agent_jobs.c"}) {
		t.Errorf("got %v, want only the implement target", got)
	}
}

// Negation binds to its own line. A path on a later line is a fresh instruction.
func TestNegationDoesNotLeakAcrossLines(t *testing.T) {
	prompt := "Do not touch src/a.c\nEdit src/b.c instead\n"
	if got := pathsCall(t, prompt); !sameStrings(got, []string{"src/b.c"}) {
		t.Errorf("got %v, want only src/b.c", got)
	}
}

// An illustrative schema shows the delegate the SHAPE of its output; the paths
// inside it are documentation, not targets.
func TestJSONExampleValuesAreNotTargets(t *testing.T) {
	prompt := `Return a packet like {"owned_files":["src/example.c"],"status":"ok"}` +
		"\nThen edit src/real.c.\n"
	if got := pathsCall(t, prompt); !sameStrings(got, []string{"src/real.c"}) {
		t.Errorf("got %v, want only src/real.c", got)
	}
}

// Prose that happens to quote a path is still an instruction, so the quote
// alone must not disqualify it -- only a JSON value position does.
func TestQuotedPathInProseIsStillATarget(t *testing.T) {
	if got := pathsCall(t, `Create "src/foo.c" with the new helper.`); !sameStrings(got,
		[]string{"src/foo.c"}) {
		t.Errorf("got %v, want src/foo.c", got)
	}
}

// A system include names a header the delegate does not own.
func TestSystemIncludesAreNotTargets(t *testing.T) {
	if got := pathsCall(t, "Add #include <sys/stat.h> to src/real.c"); !sameStrings(got,
		[]string{"src/real.c"}) {
		t.Errorf("got %v, want only src/real.c", got)
	}
}

// Everything after an evidence marker is appended context, not the brief: a
// path mentioned only in a diff was never something the delegate was asked for.
func TestEvidenceSectionsAreNotScanned(t *testing.T) {
	prompt := "Edit src/brief.c\n\n---\n## Parent Worktree Diff Evidence\n" +
		"--- a/src/evidence.c\n+++ b/src/evidence.c\n"
	if got := pathsCall(t, prompt); !sameStrings(got, []string{"src/brief.c"}) {
		t.Errorf("got %v, want only src/brief.c", got)
	}
}

// A diff's a/ and b/ prefixes are transport, not part of the path.
func TestDiffPrefixesAreStripped(t *testing.T) {
	if got := pathsCall(t, "apply to a/src/x.c and b/src/y.c"); !sameStrings(got,
		[]string{"src/x.c", "src/y.c"}) {
		t.Errorf("got %v, want the stripped paths", got)
	}
}

// An unknown extension is not source the delegate would edit.
func TestOnlyKnownSourceExtensions(t *testing.T) {
	if got := pathsCall(t, "see docs/img/diagram.png and src/real.go"); !sameStrings(got,
		[]string{"src/real.go"}) {
		t.Errorf("got %v, want only src/real.go", got)
	}
}

func TestExtractRespectsTheCap(t *testing.T) {
	var b strings.Builder
	for i := 0; i < 20; i++ {
		b.WriteString("edit src/f")
		b.WriteByte(byte('a' + i))
		b.WriteString(".c\n")
	}
	response, status := Handle(bus.ModuleInvocation{StageID: StagePaths},
		pathsRequest(b.String(), 3))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if count := binary.LittleEndian.Uint32(response[4:8]); count != 3 {
		t.Errorf("count = %d, want the requested cap of 3", count)
	}
}

func TestPathsRejectsInvalidEnvelope(t *testing.T) {
	short := pathsRequest("x", pathsMax)[:pathsHeaderLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePaths}, short); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("truncated status = %d", status)
	}
	lying := pathsRequest("x", pathsMax)
	binary.LittleEndian.PutUint32(lying[8:12], 64)
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePaths}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("length-mismatch status = %d", status)
	}
	// A cap beyond what the caller's buffer can hold must be refused, not clamped.
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePaths},
		pathsRequest("x", pathsMax+1)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("oversized-cap status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePaths, DeadlineNS: 1},
		pathsRequest("x", pathsMax)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired status = %d", status)
	}
}

// A brief that names an scp target yields the bare host path: the user@host:
// prefix is not path characters, so the token starts after the colon. Pinned
// here because the C preflight test depends on exactly this result.
func TestRemoteScpTargetYieldsTheHostPath(t *testing.T) {
	prompt := "Update aimee-server on the host. The server config lives at " +
		"admin@192.168.1.254:/mnt/media/.plugins/aimee-server/server/home/aimee.yaml; " +
		"read it over SSH and confirm the bearer token."
	want := []string{"/mnt/media/.plugins/aimee-server/server/home/aimee.yaml"}
	if got := pathsCall(t, prompt); !sameStrings(got, want) {
		t.Errorf("got %v, want %v", got, want)
	}
}

// The other two evidence markers bound the scan exactly as the diff one does.
func TestPromptFileAndValidationBundleAreNotScanned(t *testing.T) {
	for _, marker := range []string{"\n\n# Prompt File\n", "\n\n---\n## Validation Evidence Bundle\n"} {
		prompt := "Edit src/brief.c" + marker + "see src/appended.c\n"
		if got := pathsCall(t, prompt); !sameStrings(got, []string{"src/brief.c"}) {
			t.Errorf("marker %q: got %v, want only src/brief.c", marker, got)
		}
	}
}
