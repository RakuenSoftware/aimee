package skills

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func skillsRequest(count, interval int32) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(count))
	binary.LittleEndian.PutUint32(request[12:16], uint32(interval))
	return request
}

func triggerRequest(content, tool, subject string) []byte {
	request := make([]byte, triggerHeaderLen+len(content)+len(tool)+len(subject))
	binary.LittleEndian.PutUint32(request[0:4], triggerRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(content)))
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(tool)))
	binary.LittleEndian.PutUint32(request[16:20], uint32(len(subject)))
	copy(request[triggerHeaderLen:], content)
	copy(request[triggerHeaderLen+len(content):], tool)
	copy(request[triggerHeaderLen+len(content)+len(tool):], subject)
	return request
}

func TestSkillReviewIntervalParity(t *testing.T) {
	tests := []struct {
		count, interval int32
		want            uint32
	}{{12, 6, 1}, {11, 6, 0}, {0, 6, 0}, {12, 0, 0}, {-12, 6, 0}, {12, -6, 0}}
	for _, test := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageContext},
			skillsRequest(test.count, test.interval))
		if status != bus.ModuleStatusOK || len(response) != responseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != test.want {
			t.Errorf("count=%d interval=%d response=%x status=%d", test.count, test.interval,
				response, status)
		}
	}
}

func TestSkillsRejectInvalidWire(t *testing.T) {
	request := skillsRequest(12, 6)
	binary.LittleEndian.PutUint32(request[4:8], 2)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageContext}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("version status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageContext, DeadlineNS: 1},
		skillsRequest(12, 6)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}

func TestSkillTriggerParity(t *testing.T) {
	content := "---\nname: trigger-skill\ntriggers:\n  tool: [Bash]\n" +
		"  arg_pattern: [\"sleep \", \"curl \"]\n---\nPrefer condition checks.\n"
	pathContent := "---\nname: tdd\ntriggers:\n  tool: [Write, Edit]\n" +
		"  path_pattern: [\"_test.\", \"test_\"]\n---\nTest first.\n"
	tests := []struct {
		content, tool, subject string
		want                   uint32
	}{
		{content, "Bash", "sleep 5", 1},
		{content, "Bash", "echo ok", 0},
		{content, "Write", "sleep 5", 0},
		{pathContent, "Write", "src/foo_test.c", 1},
		{pathContent, "Edit", "src/test_foo.c", 1},
		{"no frontmatter", "Bash", "sleep 5", 0},
	}
	for _, test := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageTrigger},
			triggerRequest(test.content, test.tool, test.subject))
		if status != bus.ModuleStatusOK || len(response) != triggerResponseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != triggerResponseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
			binary.LittleEndian.Uint32(response[8:12]) != test.want {
			t.Errorf("tool=%q subject=%q response=%x status=%d", test.tool, test.subject,
				response, status)
		}
	}
}

func TestSkillTriggerRejectsMalformedWire(t *testing.T) {
	request := triggerRequest("---\ntriggers:\n  tool: [Bash]\n---\n", "Bash", "x")
	binary.LittleEndian.PutUint32(request[12:16], 0)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageTrigger}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("empty tool status = %d", status)
	}
	request = triggerRequest("---\ntriggers:\n  tool: [Bash]\n---\n", "Bash", "x")
	request[len(request)-1] = 0
	if _, status := Handle(bus.ModuleInvocation{StageID: StageTrigger}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("embedded NUL status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageTrigger}, request[:len(request)-1]); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("truncated status = %d", status)
	}
}
