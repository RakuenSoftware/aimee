package workspace

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func workspaceRequest(ref string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = wireVersion
	binary.LittleEndian.PutUint16(request[6:8], uint16(len(ref)))
	copy(request[8:], ref)
	return request
}

func TestWorkspaceReferenceParity(t *testing.T) {
	tests := map[string]uint32{
		"project": 1, "owner/repo": 1, "A_1/repo.name": 1,
		strings.Repeat("a", 64) + "/" + strings.Repeat("b", 64): 1,
		"../repo": 0, "owner/..": 0, "owner/repo/extra": 0, "-project": 0,
		strings.Repeat("a", 65): 0, "owner/": 0, "owner/re\x00po": 0,
	}
	for ref, want := range tests {
		response, status := Handle(ring.ModuleInvocation{StageID: StageAccess}, workspaceRequest(ref))
		if status != ring.ModuleStatusOK || len(response) != responseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != want {
			t.Errorf("%q response = %x, status = %d, want allowed %d", ref, response, status, want)
		}
	}
}

func TestWorkspaceRejectsInvalidEnvelopeAndHonorsCancellationPrecedence(t *testing.T) {
	request := workspaceRequest("owner/repo")
	request[5] = 1
	if _, status := Handle(ring.ModuleInvocation{StageID: StageAccess}, request); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("reserved-byte status = %d", status)
	}
	if _, status := Handle(ring.ModuleInvocation{StageID: StageAccess, DeadlineNS: 1},
		workspaceRequest("owner/repo/extra")); status != ring.ModuleStatusCancelled {
		t.Fatalf("expired invalid-ref status = %d", status)
	}
}
