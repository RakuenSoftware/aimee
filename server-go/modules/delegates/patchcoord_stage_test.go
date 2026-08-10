package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func patchRequestBytes(tasks []PatchTask) []byte {
	request := make([]byte, patchReqHeaderLen)
	binary.LittleEndian.PutUint32(request[0:4], patchRequestMagic)
	request[4] = wireVersion
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(tasks)))
	for _, t := range tasks {
		var hdr [20]byte
		binary.LittleEndian.PutUint32(hdr[0:4], uint32(int32(t.ID)))
		binary.LittleEndian.PutUint32(hdr[4:8], uint32(int32(t.StepID)))
		binary.LittleEndian.PutUint16(hdr[8:10], uint16(len(t.Status)))
		binary.LittleEndian.PutUint16(hdr[10:12], uint16(len(t.Error)))
		binary.LittleEndian.PutUint32(hdr[12:16], uint32(len(t.Files)))
		binary.LittleEndian.PutUint32(hdr[16:20], uint32(len(t.Result)))
		request = append(request, hdr[:]...)
		request = append(request, t.Status...)
		request = append(request, t.Error...)
		request = append(request, t.Files...)
		request = append(request, t.Result...)
	}
	return request
}

func TestPatchStageRoundTrip(t *testing.T) {
	tasks := []PatchTask{
		{ID: 1, StepID: 7, Status: "done", Files: `["src/a.c"]`,
			Result: packet(`"src/a.c"`, passedTest)},
		{ID: 2, StepID: 8, Status: "done", Files: `["src/a.c"]`,
			Result: packet(`"src/a.c"`, passedTest)},
	}
	response, status := Handle(bus.ModuleInvocation{StageID: StagePatchCoord},
		patchRequestBytes(tasks))
	if status != bus.ModuleStatusOK ||
		binary.LittleEndian.Uint32(response[0:4]) != patchResponseMagic {
		t.Fatalf("status %d, %d bytes", status, len(response))
	}
	want := BuildPatchReport(tasks)

	count := int(binary.LittleEndian.Uint32(response[4:8]))
	if count != len(want.Tasks) {
		t.Fatalf("task count = %d, want %d", count, len(want.Tasks))
	}
	if len(response) != patchRespHeaderLen+count*patchTaskRecordLen {
		t.Fatalf("response length %d does not match %d tasks", len(response), count)
	}
	// Reviewable is the field a supervisor acts on, and the overlap must have
	// stopped exactly one packet.
	if got := int(binary.LittleEndian.Uint32(response[28:32])); got != want.Reviewable {
		t.Errorf("reviewable = %d, want %d", got, want.Reviewable)
	}

	at := patchRespHeaderLen + patchTaskRecordLen // second task record
	off := 9 * 4
	second := decodeFixed(response[at+off+patchStateLen : at+off+2*patchStateLen])
	if second != "needs_supervisor" {
		t.Errorf("second packet state = %q, want needs_supervisor", second)
	}
	if overlap := int(binary.LittleEndian.Uint32(response[at+24 : at+28])); overlap != 1 {
		t.Errorf("overlap task id = %d, want 1", overlap)
	}
}

func TestPatchStageRejectsInvalidEnvelope(t *testing.T) {
	good := patchRequestBytes([]PatchTask{{ID: 1, Status: "pending"}})

	if _, status := Handle(bus.ModuleInvocation{StageID: StagePatchCoord},
		good[:patchReqHeaderLen-1]); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("truncated status = %d", status)
	}

	lying := patchRequestBytes([]PatchTask{{ID: 1, Status: "pending"}})
	binary.LittleEndian.PutUint32(lying[8:12], 99)
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePatchCoord}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("overrunning task count status = %d", status)
	}

	trailing := append(patchRequestBytes(nil), 0)
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePatchCoord}, trailing); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("trailing-byte status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StagePatchCoord, DeadlineNS: 1},
		good); status != bus.ModuleStatusCancelled {
		t.Errorf("expired status = %d", status)
	}
}
