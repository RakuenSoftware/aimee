package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Answering "may this delegate write?" in one call.
//
// The caller used to compose this from two answers -- the role's default and a
// prompt rule of its own. Composing it here means the boolean that reaches the
// worktree plan (stage 11) and the container spec (stage 12) is the same
// boolean, decided once. Those stages take the composed answer precisely
// because it is the one fact that must agree across them.

const (
	StageMayWrite uint32 = 15
	EventMayWrite uint32 = 6671

	mayWriteRequestMagic  uint32 = 0x51575744 /* "DWWQ" */
	mayWriteResponseMagic uint32 = 0x53575744 /* "DWWS" */
	mayWriteReqHeaderLen         = 16
	mayWriteResponseLen          = 16

	// A brief is carried whole because the rule reads its text; there is
	// nothing smaller to send that preserves the answer.
	mayWritePromptMax = 1 << 20
)

// handleMayWrite returns the composed permission, and its two halves.
//
// The halves travel too, not for the decision -- the caller must use MayWrite --
// but because "the delegate could not edit anything" is otherwise indebuggable:
// an operator needs to see whether the role or the brief withheld it.
func handleMayWrite(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < mayWriteReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != mayWriteRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	roleLen := int(binary.LittleEndian.Uint32(request[8:12]))
	promptLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if roleLen > roleMax || promptLen > mayWritePromptMax {
		return nil, bus.ModuleStatusInvalidRequest
	}

	c := &economicsCursor{buf: request, at: mayWriteReqHeaderLen}
	role := c.str(roleLen)
	prompt := c.str(promptLen)
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	roleWrites := RoleIsWrite(role)
	promptWrites := PromptAllowsWrites(prompt)

	response := make([]byte, mayWriteResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], mayWriteResponseMagic)
	putBool(response[4:8], roleWrites && promptWrites)
	putBool(response[8:12], roleWrites)
	putBool(response[12:16], promptWrites)
	return response, bus.ModuleStatusOK
}
