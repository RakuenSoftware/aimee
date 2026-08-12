package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// A request builder, mirroring what the C encoder writes.
type launchReq struct{ w wireWriter }

func newLaunchReq(maxConcurrent int, schema, title string) *launchReq {
	r := &launchReq{}
	r.w.u32(launchPlanRequestMagic)
	r.w.u32(uint32(wireVersion))
	r.w.u32(uint32(maxConcurrent))
	r.w.str(schema)
	r.w.str(title)
	return r
}

func (r *launchReq) missing(files ...string) *launchReq {
	r.w.strings(files)
	return r
}

func (r *launchReq) packets(n int) *launchReq {
	r.w.u32(uint32(n))
	return r
}

func (r *launchReq) packet(id, title, objective, role, schema string, files ...LaunchOwnedFile) *launchReq {
	r.w.str(id)
	r.w.str(title)
	r.w.str(objective)
	r.w.str(role)
	r.w.str(schema)
	r.w.u32(uint32(len(files)))
	for _, f := range files {
		r.w.str(f.Path)
		if f.Exists {
			r.w.u32(1)
		} else {
			r.w.u32(0)
		}
		r.w.strings(f.Candidates)
	}
	return r
}

func (r *launchReq) bytes() []byte { return r.w.buf }

// A minimal reader for what the stage returns.
type launchResp struct {
	err           string
	maxConcurrent int
	steps         []LaunchStep
	tasks         []LaunchTask
	repairs       []LaunchRepair
	warnings      []string
}

func decodeLaunchResponse(t *testing.T, b []byte) launchResp {
	t.Helper()
	r := &wireReader{buf: b}
	if got := r.u32(); got != launchPlanResponseMagic {
		t.Fatalf("bad response magic %#x", got)
	}
	out := launchResp{err: r.str()}
	out.maxConcurrent = int(r.u32())

	for n := r.count(launchPlanMaxPackets); n > 0; n-- {
		out.steps = append(out.steps, LaunchStep{
			Action: r.str(), Precondition: r.str(), SuccessPredicate: r.str(), Rollback: r.str(),
		})
	}
	for n := r.count(launchPlanMaxPackets); n > 0; n-- {
		out.tasks = append(out.tasks, LaunchTask{
			OwnedFiles: r.strings(launchPlanMaxFiles), Role: r.str(), Prompt: r.str(),
		})
	}
	for n := r.count(launchPlanMaxFiles); n > 0; n-- {
		out.repairs = append(out.repairs, LaunchRepair{From: r.str(), To: r.str()})
	}
	out.warnings = r.strings(launchPlanMaxFiles)

	if !r.done() {
		t.Fatalf("response has %d unread bytes", len(b)-r.at)
	}
	return out
}

func TestLaunchPlanStageRoundTrip(t *testing.T) {
	req := newLaunchReq(4, "delegate_plan_v1", "a plan").
		missing().
		packets(2).
		packet("p1", "first", "do the first thing", "code", "delegate_result_v1",
			LaunchOwnedFile{Path: "src/a.c", Exists: true}).
		packet("rev", "review", "check it", "review", "delegate_result_v1")

	response, status := handleLaunchPlan(bus.ModuleInvocation{}, req.bytes())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	d := decodeLaunchResponse(t, response)
	if d.err != "" {
		t.Fatalf("unexpected error %q", d.err)
	}
	if d.maxConcurrent != 4 {
		t.Errorf("max concurrent: want 4, got %d", d.maxConcurrent)
	}
	if len(d.steps) != 1 || len(d.tasks) != 1 {
		t.Fatalf("reviewer should not launch: %d steps, %d tasks", len(d.steps), len(d.tasks))
	}
	if d.steps[0].Action != "first" || d.tasks[0].Role != "code" {
		t.Errorf("wrong step/task: %+v %+v", d.steps[0], d.tasks[0])
	}
	if len(d.tasks[0].OwnedFiles) != 1 || d.tasks[0].OwnedFiles[0] != "src/a.c" {
		t.Errorf("owned files: %+v", d.tasks[0].OwnedFiles)
	}
}

// A rejected plan still round-trips: the caller needs the reason, and must get
// no steps or tasks to write.
func TestLaunchPlanStageCarriesTheRefusal(t *testing.T) {
	req := newLaunchReq(0, "delegate_plan_v2", "a plan").missing().packets(0)
	response, status := handleLaunchPlan(bus.ModuleInvocation{}, req.bytes())
	if status != bus.ModuleStatusOK {
		t.Fatalf("a refusal is a valid answer, not a bad request: %v", status)
	}
	d := decodeLaunchResponse(t, response)
	if d.err == "" {
		t.Fatal("want a refusal")
	}
	if len(d.steps) != 0 || len(d.tasks) != 0 {
		t.Error("a refused plan must carry nothing to write")
	}
}

func TestLaunchPlanStageReportsRepairsAndWarnings(t *testing.T) {
	req := newLaunchReq(0, "delegate_plan_v1", "a plan").
		missing().
		packets(1).
		packet("p1", "first", "do it", "code", "delegate_result_v1",
			LaunchOwnedFile{Path: "util.c", Candidates: []string{"src/util.c"}},
			LaunchOwnedFile{Path: "src/new.c"})

	response, status := handleLaunchPlan(bus.ModuleInvocation{}, req.bytes())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	d := decodeLaunchResponse(t, response)
	if len(d.repairs) != 1 || d.repairs[0].To != "src/util.c" {
		t.Errorf("repairs: %+v", d.repairs)
	}
	if len(d.warnings) != 1 {
		t.Errorf("warnings: %+v", d.warnings)
	}
	if len(d.tasks[0].OwnedFiles) != 2 || d.tasks[0].OwnedFiles[0] != "src/util.c" {
		t.Errorf("task should carry the repaired path: %+v", d.tasks[0].OwnedFiles)
	}
}

// A request this module would read differently from how the caller wrote it is
// refused outright. Launching the part that parsed would create a job missing
// packets nobody noticed were dropped.
func TestLaunchPlanStageRejectsMalformedRequests(t *testing.T) {
	good := newLaunchReq(1, "delegate_plan_v1", "a plan").
		missing().
		packets(1).
		packet("p1", "first", "do it", "code", "delegate_result_v1",
			LaunchOwnedFile{Path: "a.c", Exists: true}).
		bytes()

	badMagic := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], launchPlanRequestMagic+1)

	badVersion := append([]byte(nil), good...)
	badVersion[4] = wireVersion + 1

	reservedSet := append([]byte(nil), good...)
	reservedSet[5] = 1 // the bytes above the version are reserved and must be zero

	overlong := append([]byte(nil), good...)
	overlong = append(overlong, 0)

	truncated := good[:len(good)-1]

	hugeCount := newLaunchReq(1, "delegate_plan_v1", "a plan").missing().bytes()
	hugeCount = append(hugeCount, 0xff, 0xff, 0xff, 0xff) // packet count past its ceiling

	for name, request := range map[string][]byte{
		"bad magic":      badMagic,
		"bad version":    badVersion,
		"reserved byte":  reservedSet,
		"trailing bytes": overlong,
		"truncated":      truncated,
		"count ceiling":  hugeCount,
		"empty":          nil,
		"header only":    good[:8],
	} {
		if _, status := handleLaunchPlan(bus.ModuleInvocation{}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: want InvalidRequest, got %v", name, status)
		}
	}
}

func TestLaunchPlanStageHonoursCancellation(t *testing.T) {
	req := newLaunchReq(1, "delegate_plan_v1", "a plan").missing().packets(0).bytes()
	// An expired deadline is a cancellation: a plan decided after its caller
	// stopped waiting would be written by nobody.
	if _, status := handleLaunchPlan(bus.ModuleInvocation{DeadlineNS: 1}, req); status != bus.ModuleStatusCancelled {
		t.Errorf("want Cancelled, got %v", status)
	}
}
