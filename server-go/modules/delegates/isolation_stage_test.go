package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func isolationRequest(report string, probeFailed, require bool) []byte {
	out := make([]byte, isolationReqHeaderLen)
	binary.LittleEndian.PutUint32(out[0:4], isolationRequestMagic)
	out[4] = wireVersion
	if probeFailed {
		out[5] |= isolationFlagProbeFailed
	}
	if require {
		out[5] |= isolationFlagRequire
	}
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(report)))
	return append(out, report...)
}

func callIsolation(t *testing.T, report string, probeFailed, require bool) (bool, bool, string) {
	refuse, warn, _, reason := callIsolationFull(t, report, probeFailed, require)
	return refuse, warn, reason
}

func callIsolationFull(t *testing.T, report string, probeFailed, require bool) (bool, bool, bool, string) {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageIsolation},
		isolationRequest(report, probeFailed, require))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if len(response) < 16 || binary.LittleEndian.Uint32(response[0:4]) != isolationResponseMagic {
		t.Fatal("bad response header")
	}
	return binary.LittleEndian.Uint32(response[4:8]) == 1,
		binary.LittleEndian.Uint32(response[8:12]) == 1,
		binary.LittleEndian.Uint32(response[12:16]) == 1,
		string(response[16:])
}

// A confirmed breach is always reported -- the container can reach the network
// regardless of what was asked for, so the egress allowlist is not in force --
// but it refuses only when isolation is required. Refusing unconditionally
// would turn a runtime that has always ignored the flag into a total outage.
func TestIsolationStageBreachRefusesOnlyWhenRequired(t *testing.T) {
	refuse, _, reason := callIsolation(t, "bridge=172.17.0.2;", false, true)
	if !refuse {
		t.Error("a confirmed breach did not refuse under require_isolation")
	}
	if !strings.Contains(reason, "network") {
		t.Errorf("reason does not say what was observed: %q", reason)
	}

	refuse, warn, reason := callIsolation(t, "bridge=172.17.0.2;", false, false)
	if refuse {
		t.Error("a breach refused without require_isolation")
	}
	if !warn {
		t.Error("a breach passed silently: the sandbox is not a sandbox and nothing said so")
	}
	if !strings.Contains(reason, "require_isolation") {
		t.Errorf("reason does not tell the operator how to make it refuse: %q", reason)
	}
}

// Docker prints nothing when the network map is empty, and that IS the isolated
// answer -- not an unreadable one.
func TestIsolationStageEmptyReportIsIsolated(t *testing.T) {
	for _, report := range []string{"", "   ", "\n"} {
		refuse, warn, _ := callIsolation(t, report, false, true)
		if refuse || warn {
			t.Errorf("report %q: refuse=%v warn=%v, want a clean pass", report, refuse, warn)
		}
	}
}

// A network named "none" with no address is what --network none looks like.
func TestIsolationStageNoneNetworkIsIsolated(t *testing.T) {
	if refuse, warn, _ := callIsolation(t, "none=;", false, true); refuse || warn {
		t.Errorf("refuse=%v warn=%v, want a clean pass", refuse, warn)
	}
}

// The heart of it: a probe that could not answer is NOT an isolated container.
// "The probe failed" and "the sandbox is open" are indistinguishable from here,
// so an operator who requires isolation gets a refusal.
func TestIsolationStageUnknownRefusesOnlyWhenRequired(t *testing.T) {
	refuse, warn, reason := callIsolation(t, "", true, true)
	if !refuse {
		t.Error("a failed probe was allowed to run under require_isolation")
	}
	if reason == "" {
		t.Error("no reason given for the refusal")
	}

	refuse, warn, _ = callIsolation(t, "", true, false)
	if refuse {
		t.Error("a failed probe refused without require_isolation, which would make a flaky " +
			"runtime look like a broken delegate")
	}
	if !warn {
		t.Error("a failed probe passed silently")
	}
}

// A line that does not match the contract is not evidence of safety.
func TestIsolationStageUnparseableIsNotIsolated(t *testing.T) {
	if refuse, _, _ := callIsolation(t, "garbage without an equals", false, true); !refuse {
		t.Error("an unparseable report was treated as isolated")
	}
}

func TestIsolationStageRejectsMalformedRequests(t *testing.T) {
	good := isolationRequest("none=;", false, true)

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:8],
		"trailing byte": append(append([]byte{}, good...), 0),
		"truncated":     good[:len(good)-1],
	}
	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	badFlags := append([]byte{}, good...)
	badFlags[5] = 0xFF
	cases["unknown flags"] = badFlags

	overrun := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(overrun[8:12], 1<<20)
	cases["report length overruns"] = overrun

	for name, request := range cases {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageIsolation}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestIsolationStageHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageIsolation, DeadlineNS: 1}
	if _, status := Handle(invocation, isolationRequest("none=;", false, true)); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}

// A breach that runs anyway is an ERROR, not a caution. Reporting it at the
// same level as "the probe was flaky" is how it gets scrolled past -- and the
// caller cannot tell the two apart from the wording alone.
func TestIsolationStageBreachIsAnErrorEvenWhenItRuns(t *testing.T) {
	refuse, warn, isErr, _ := callIsolationFull(t, "bridge=172.17.0.2;", false, false)
	if refuse {
		t.Error("a breach refused without require_isolation")
	}
	if !warn || !isErr {
		t.Errorf("breach reported as warn=%v error=%v, want both", warn, isErr)
	}

	// A merely unverifiable probe is NOT an error when isolation is optional.
	_, warn, isErr, _ = callIsolationFull(t, "", true, false)
	if !warn {
		t.Error("a failed probe said nothing")
	}
	if isErr {
		t.Error("a flaky probe was raised to error, which is what makes real breaches ignorable")
	}
}
