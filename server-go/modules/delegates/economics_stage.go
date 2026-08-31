package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	StageEconomics uint32 = 8
	EventEconomics uint32 = 6664

	economicsRequestMagic  uint32 = 0x51434544 /* "DECQ" */
	economicsResponseMagic uint32 = 0x53434544 /* "DECS" */
	economicsReqHeaderLen         = 16
	economicsVerdictLen           = 32
	economicsAdviceLen            = 256
	economicsLabelLen             = 64
	// The two labels ride along so the verdict has exactly one rendering.
	// A caller formatting them itself would be a second copy that can
	// disagree with the verdict it is captioning.
	economicsResponseLen = 4 + 19*4 + economicsVerdictLen + economicsAdviceLen +
		2*economicsLabelLen
	economicsMaxTasks  = 4096
	economicsMaxAgents = 4096
)

type economicsCursor struct {
	buf []byte
	at  int
	bad bool
}

func (c *economicsCursor) u16() int {
	if c.bad || c.at+2 > len(c.buf) {
		c.bad = true
		return 0
	}
	v := int(binary.LittleEndian.Uint16(c.buf[c.at : c.at+2]))
	c.at += 2
	return v
}

func (c *economicsCursor) u32() int {
	if c.bad || c.at+4 > len(c.buf) {
		c.bad = true
		return 0
	}
	v := int(binary.LittleEndian.Uint32(c.buf[c.at : c.at+4]))
	c.at += 4
	return v
}

func (c *economicsCursor) str(n int) string {
	if c.bad || n < 0 || c.at+n > len(c.buf) {
		c.bad = true
		return ""
	}
	s := string(c.buf[c.at : c.at+n])
	c.at += n
	return s
}

// handleEconomics aggregates a coordinated run's cost to the supervisor.
//
// Tasks arrive as content: the four fields the rule reads, nothing else from
// the row. Agent tiers arrive the same way, because which seat is dear is the
// caller's configuration, not this module's state.
func handleEconomics(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < economicsReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != economicsRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	taskCount := int(binary.LittleEndian.Uint32(request[8:12]))
	agentCount := int(binary.LittleEndian.Uint32(request[12:16]))
	if taskCount > economicsMaxTasks || agentCount > economicsMaxAgents {
		return nil, bus.ModuleStatusInvalidRequest
	}

	c := &economicsCursor{buf: request, at: economicsReqHeaderLen}
	tasks := make([]EconomicsTask, 0, taskCount)
	for i := 0; i < taskCount; i++ {
		statusLen, claimedLen := c.u16(), c.u16()
		filesLen, resultLen := c.u32(), c.u32()
		task := EconomicsTask{
			Status:    c.str(statusLen),
			ClaimedBy: c.str(claimedLen),
			Files:     c.str(filesLen),
			Result:    c.str(resultLen),
		}
		if c.bad {
			return nil, bus.ModuleStatusInvalidRequest
		}
		tasks = append(tasks, task)
	}
	agents := make([]AgentTier, 0, agentCount)
	for i := 0; i < agentCount; i++ {
		nameLen := c.u16()
		name := c.str(nameLen)
		tier := int(int32(c.u32()))
		if c.bad {
			return nil, bus.ModuleStatusInvalidRequest
		}
		agents = append(agents, AgentTier{Name: name, Tier: tier})
	}
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	report := BuildEconomicsReport(tasks, agents)

	response := make([]byte, economicsResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], economicsResponseMagic)
	fields := []int{
		report.DelegateCount,
		report.TierCounts[0], report.TierCounts[1], report.TierCounts[2], report.TierCounts[3],
		report.UnknownTierCount,
		report.PromptTokensTotal, report.CompletionTokensTotal,
		report.DelegateTokensEstimated, report.TokenizedDelegateResults,
		report.SupervisorPromptTokensEstimated,
		report.HandoffCount, report.ValidHandoffs, report.InvalidHandoffs,
		report.FocusedTestsRunByDelegates, report.DelegatesWithFocusedTests,
		report.ManualIntegrationEvents, report.SupervisorActionsRequired,
		report.ReviewerFindingsBlocking,
	}
	for i, v := range fields {
		binary.LittleEndian.PutUint32(response[4+i*4:8+i*4], uint32(int32(v)))
	}
	at := 4 + len(fields)*4
	putFixed(response[at:at+economicsVerdictLen], report.Verdict)
	at += economicsVerdictLen
	putFixed(response[at:at+economicsAdviceLen], report.Recommendation)
	at += economicsAdviceLen
	putFixed(response[at:at+economicsLabelLen], EconomicsVerdictText(report.Verdict))
	putFixed(response[at+economicsLabelLen:], EconomicsCostModelLabel())
	return response, bus.ModuleStatusOK
}
