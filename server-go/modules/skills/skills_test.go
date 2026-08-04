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
