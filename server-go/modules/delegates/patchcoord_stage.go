package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	StagePatchCoord uint32 = 9
	EventPatchCoord uint32 = 6665

	patchRequestMagic  uint32 = 0x51435044 /* "DPCQ" */
	patchResponseMagic uint32 = 0x53435044 /* "DPCS" */
	patchReqHeaderLen         = 16
	patchRespHeaderLen        = 4 + 18*4 + patchStateLen + patchNextCmdLen
	// task_id, step_id, handoff_valid, changed, passed, outside, overlap,
	// stale, actions -- then the three fixed strings.
	patchTaskRecordLen = 9*4 + patchStateLen*3 + patchNoteLen
)

func handlePatchCoord(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < patchReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != patchRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	taskCount := int(binary.LittleEndian.Uint32(request[8:12]))
	if taskCount > patchMaxTasks {
		return nil, bus.ModuleStatusInvalidRequest
	}

	c := &economicsCursor{buf: request, at: patchReqHeaderLen}
	tasks := make([]PatchTask, 0, taskCount)
	for i := 0; i < taskCount; i++ {
		id, stepID := int(int32(c.u32())), int(int32(c.u32()))
		statusLen := c.u16()
		errorLen := c.u16()
		filesLen, resultLen := c.u32(), c.u32()
		task := PatchTask{
			ID:     id,
			StepID: stepID,
			Status: c.str(statusLen),
			Error:  c.str(errorLen),
			Files:  c.str(filesLen),
			Result: c.str(resultLen),
		}
		if c.bad {
			return nil, bus.ModuleStatusInvalidRequest
		}
		tasks = append(tasks, task)
	}
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	report := BuildPatchReport(tasks)

	response := make([]byte, patchRespHeaderLen, patchRespHeaderLen+len(report.Tasks)*patchTaskRecordLen)
	binary.LittleEndian.PutUint32(response[0:4], patchResponseMagic)
	fields := []int{
		len(report.Tasks), report.ImplementationPackets,
		report.Planned, report.Running, report.Returned, report.Verified,
		report.Reviewable, report.Accepted, report.Failed, report.NeedsSupervisor,
		report.InvalidHandoffs, report.OutsideOwnershipTouches, report.PatchOverlaps,
		report.StaleWorktrees, report.FocusedTestsPassed,
		report.ReviewerPackets, report.ReviewerBlockingFindings,
		report.ReviewerOwnerPacketRoutes,
	}
	for i, v := range fields {
		binary.LittleEndian.PutUint32(response[4+i*4:8+i*4], uint32(int32(v)))
	}
	at := 4 + len(fields)*4
	putFixed(response[at:at+patchStateLen], report.ReviewerStatus)
	putFixed(response[at+patchStateLen:], report.RecommendedNextCommand)

	for _, tr := range report.Tasks {
		rec := make([]byte, patchTaskRecordLen)
		nums := []int{
			tr.TaskID, tr.StepID, boolInt(tr.HandoffValid), tr.ChangedFilesCount,
			tr.PassedTests, tr.OutsideOwnershipCount, tr.OverlapTaskID,
			boolInt(tr.StaleBase), tr.SupervisorActions,
		}
		for i, v := range nums {
			binary.LittleEndian.PutUint32(rec[i*4:i*4+4], uint32(int32(v)))
		}
		off := len(nums) * 4
		putFixed(rec[off:off+patchStateLen], tr.TaskStatus)
		putFixed(rec[off+patchStateLen:off+2*patchStateLen], tr.PatchState)
		putFixed(rec[off+2*patchStateLen:off+3*patchStateLen], tr.HandoffStatus)
		putFixed(rec[off+3*patchStateLen:], tr.Note)
		response = append(response, rec...)
	}
	return response, bus.ModuleStatusOK
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}
