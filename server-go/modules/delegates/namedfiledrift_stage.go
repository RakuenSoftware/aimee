package delegates

import "github.com/JBailes/aimee/server-go/bus"

// Did the delegate touch the files its brief named?
//
// Length-prefixed like the launch-plan stage, and for the same reason: this
// carries prose and a variable number of paths. Every read is bounds-checked and
// every count capped; a request read differently from how it was written would
// produce a verdict about a different set of files than the caller asked about.

const (
	StageNamedFileDrift uint32 = 21
	EventNamedFileDrift uint32 = 6677

	driftRequestMagic  uint32 = 0x51465244 /* "DRFQ" */
	driftResponseMagic uint32 = 0x53465244 /* "DRFS" */

	driftMaxPaths                 = 256
	driftMaxIndexHits             = 64
	driftFlagWritesAllowed uint32 = 1 << 0
	driftFlagPathExist     uint32 = 1 << 0
	driftFlagPathDiff      uint32 = 1 << 1
)

func handleNamedFileDrift(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	r := &wireReader{buf: request}
	if r.u32() != driftRequestMagic || r.u32() != uint32(wireVersion) {
		return nil, bus.ModuleStatusInvalidRequest
	}

	flags := r.u32()
	if flags&^driftFlagWritesAllowed != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}

	facts := DriftFacts{
		Prompt:        r.str(),
		Response:      r.str(),
		WorktreePath:  r.str(),
		WritesAllowed: flags&driftFlagWritesAllowed != 0,
	}

	count := r.count(driftMaxPaths)
	for i := 0; i < count && !r.bad; i++ {
		p := NamedPath{Path: r.str()}
		pathFlags := r.u32()
		if pathFlags&^(driftFlagPathExist|driftFlagPathDiff) != 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		p.Exists = pathFlags&driftFlagPathExist != 0
		p.InDiff = pathFlags&driftFlagPathDiff != 0
		p.IndexHitFiles = r.strings(driftMaxIndexHits)
		facts.Paths = append(facts.Paths, p)
	}

	if !r.done() {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	v := JudgeNamedFileDrift(facts)

	w := &wireWriter{}
	w.u32(driftResponseMagic)
	w.u32(uint32(v.Severity))
	w.str(v.Message)
	return w.buf, bus.ModuleStatusOK
}
