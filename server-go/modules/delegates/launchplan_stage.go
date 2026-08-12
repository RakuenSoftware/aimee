package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// The wire for the launch decision.
//
// Unlike the delegate stages before it, this one carries prose -- titles,
// objectives, paths, and the briefs it returns -- so it is length-prefixed
// rather than fixed-width. Every read is bounds-checked against the remaining
// buffer and every count against a ceiling: a truncated or hostile request must
// be rejected, never partially believed, because a partially-read plan would
// launch a subset of the work and report success.

const (
	StageLaunchPlan uint32 = 19
	EventLaunchPlan uint32 = 6675

	launchPlanRequestMagic  uint32 = 0x514c5044 /* "DPLQ" */
	launchPlanResponseMagic uint32 = 0x534c5044 /* "DPLS" */

	// Ceilings. Generous against real plans, bounded against a malformed
	// length that would otherwise make us allocate on a whim.
	launchPlanMaxPackets    = 4096
	launchPlanMaxFiles      = 4096
	launchPlanMaxCandidates = 256
	launchPlanMaxStringLen  = 1 << 20
)

// A cursor over the request, which refuses to read past its end.
type wireReader struct {
	buf []byte
	at  int
	bad bool
}

func (r *wireReader) u32() uint32 {
	if r.bad || r.at+4 > len(r.buf) {
		r.bad = true
		return 0
	}
	v := binary.LittleEndian.Uint32(r.buf[r.at : r.at+4])
	r.at += 4
	return v
}

func (r *wireReader) str() string {
	n := int(r.u32())
	if r.bad || n < 0 || n > launchPlanMaxStringLen || r.at+n > len(r.buf) {
		r.bad = true
		return ""
	}
	s := string(r.buf[r.at : r.at+n])
	r.at += n
	return s
}

// count reads a length and rejects it against a ceiling before any allocation.
func (r *wireReader) count(max int) int {
	n := int(r.u32())
	if r.bad || n < 0 || n > max {
		r.bad = true
		return 0
	}
	return n
}

func (r *wireReader) strings(max int) []string {
	n := r.count(max)
	if r.bad || n == 0 {
		return nil
	}
	out := make([]string, 0, n)
	for i := 0; i < n; i++ {
		out = append(out, r.str())
		if r.bad {
			return nil
		}
	}
	return out
}

func (r *wireReader) done() bool { return !r.bad && r.at == len(r.buf) }

type wireWriter struct{ buf []byte }

func (w *wireWriter) u32(v uint32) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	w.buf = append(w.buf, b[:]...)
}

func (w *wireWriter) str(s string) {
	w.u32(uint32(len(s)))
	w.buf = append(w.buf, s...)
}

func (w *wireWriter) strings(items []string) {
	w.u32(uint32(len(items)))
	for _, s := range items {
		w.str(s)
	}
}

// handleLaunchPlan reads a plan and returns the steps, tasks, repairs and
// warnings it becomes -- or the reason it must not launch.
func handleLaunchPlan(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	r := &wireReader{buf: request}
	// Same header bytes as every other delegate stage: magic, then the version
	// in byte 4. The three bytes above it are reserved and must be zero, so a
	// future field cannot be introduced without this module noticing.
	if r.u32() != launchPlanRequestMagic || r.u32() != uint32(wireVersion) {
		return nil, bus.ModuleStatusInvalidRequest
	}

	maxConcurrent := int(int32(r.u32()))
	plan := LaunchPlan{Schema: r.str(), Title: r.str()}
	plan.MissingOwnedFiles = r.strings(launchPlanMaxFiles)

	packetCount := r.count(launchPlanMaxPackets)
	for i := 0; i < packetCount && !r.bad; i++ {
		p := LaunchPacket{
			ID:            r.str(),
			Title:         r.str(),
			Objective:     r.str(),
			Role:          r.str(),
			HandoffSchema: r.str(),
		}
		fileCount := r.count(launchPlanMaxFiles)
		for j := 0; j < fileCount && !r.bad; j++ {
			f := LaunchOwnedFile{Path: r.str()}
			f.Exists = r.u32() != 0
			f.Candidates = r.strings(launchPlanMaxCandidates)
			p.OwnedFiles = append(p.OwnedFiles, f)
		}
		plan.Packets = append(plan.Packets, p)
	}

	// A request with bytes left over is one this module read differently from
	// how the caller wrote it. Refuse rather than launch part of a plan.
	if !r.done() {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	d := PlanLaunch(plan, maxConcurrent)

	w := &wireWriter{}
	w.u32(launchPlanResponseMagic)
	w.str(d.Error)
	w.u32(uint32(d.MaxConcurrent))

	w.u32(uint32(len(d.Steps)))
	for _, s := range d.Steps {
		w.str(s.Action)
		w.str(s.Precondition)
		w.str(s.SuccessPredicate)
		w.str(s.Rollback)
	}

	w.u32(uint32(len(d.Tasks)))
	for _, t := range d.Tasks {
		w.strings(t.OwnedFiles)
		w.str(t.Role)
		w.str(t.Prompt)
	}

	w.u32(uint32(len(d.Repairs)))
	for _, rep := range d.Repairs {
		w.str(rep.From)
		w.str(rep.To)
	}

	w.strings(d.Warnings)
	return w.buf, bus.ModuleStatusOK
}
