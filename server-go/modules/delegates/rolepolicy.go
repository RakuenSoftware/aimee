package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// What a delegate ROLE implies about how it should be run.
//
// These are fixed policy, not operator configuration: a role's turn cap comes
// from its template frontmatter and stays with the caller, but whether a role
// writes, whether it needs tools to do its job at all, and whether its output
// is safe to cache are properties of the role itself.

const (
	StageRolePolicy uint32 = 10
	EventRolePolicy uint32 = 6666

	rolePolicyRequestMagic  uint32 = 0x514c5244 /* "DRLQ" */
	rolePolicyResponseMagic uint32 = 0x534c5244 /* "DRLS" */
	rolePolicyRequestLen           = 16 + roleMax + 1
	rolePolicyResponseLen          = 28
)

// canonicalRole folds an alias onto the role it names. Doing it here rather
// than asking back out means the policy answers below are always computed from
// the same spelling the caller's role resolves to.
func canonicalRole(role string) string {
	if canonical, ok := aliases[role]; ok {
		return canonical
	}
	return role
}

// RoleIsWrite reports whether the role changes the repository.
func RoleIsWrite(role string) bool {
	switch canonicalRole(role) {
	case "code", "refactor":
		return true
	}
	return false
}

// RoleEnablesToolsByDefault reports whether a role needs tools even without an
// explicit request.
//
// A write role cannot do its job without a filesystem, and left tools-off it
// cannot fail visibly either: asked to implement, an agent with no file tools
// returns a per-file diff summary of code it never wrote. Tools-on is the only
// honest default; an explicit --no-tools still overrides it.
func RoleEnablesToolsByDefault(role string) bool {
	if role == "" {
		return false
	}
	canonical := canonicalRole(role)
	if RoleIsWrite(canonical) {
		return true
	}
	switch canonical {
	case "review", "search", "execute", "diagnose", "validate",
		// Novel-mode read-only checks inspect the world bible by default.
		"continuity", "beat-check":
		return true
	}
	return false
}

// RoleResultCacheEnabled reports whether a response may be reused keyed only by
// (role, prompt).
//
// Opt-in, and only for pure text transforms. Repository inspection, execution
// and custom roles must never cache: the same prompt can refer to a changed
// working tree, so a cached answer would describe a repository that no longer
// exists.
func RoleResultCacheEnabled(role string) bool {
	switch canonicalRole(role) {
	case "summarize", "format", "draft":
		return true
	}
	return false
}

// RoleNeedsParentDiffEvidence reports whether a read-only inspection role should
// be grounded in the PARENT worktree's uncommitted diff.
//
// These roles run against an isolated checkout whose own `git diff` is clean or
// different, so left to themselves they report on the wrong tree -- or announce
// there is nothing to review while the work sits uncommitted next door. Copying
// the parent's diff in makes the thing they were asked about visible.
//
// This answers the ROLE half only, and the caller composes the rest: the
// evidence is suppressed for a delegate that may WRITE (it is producing the
// diff, not reviewing one) and for a review target that arrived in the prompt
// (an unrelated worktree diff is then competing evidence, which has made plan
// reviewers demand implementation that was never in scope).
//
// Those two are conditions of the INVOCATION, not properties of the role, and
// this stage answers one question per role for every op at once -- so folding
// them in here would mean computing them for callers that did not ask.
func RoleNeedsParentDiffEvidence(role string) bool {
	switch canonicalRole(role) {
	case "validate", "review", "diagnose", "test":
		return true
	}
	return false
}

// RoleAutoToolsForInvocation applies the role default to one invocation.
//
// A single-turn run is a final-answer smoke probe, so it gets no implicit
// tools; asking for them explicitly still wins.
func RoleAutoToolsForInvocation(role string, maxTurns int, explicitTools bool) bool {
	if explicitTools {
		return true
	}
	if maxTurns == 1 {
		return false
	}
	return RoleEnablesToolsByDefault(role)
}

// RoleFinalAfterTurns is the turn at which an inspection role should stop using
// tools and answer, or -1 when the role has no early-final policy.
func RoleFinalAfterTurns(role string) int {
	switch canonicalRole(role) {
	case "validate":
		return 8
	case "search":
		return 10
	case "diagnose":
		return 12
	}
	return -1
}

func handleRolePolicy(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != rolePolicyRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != rolePolicyRequestMagic ||
		request[4] != wireVersion || request[5] > 1 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	explicitTools := request[5] == 1
	maxTurns := int(int32(binary.LittleEndian.Uint32(request[8:12])))
	roleLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if roleLen > roleMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	role := string(request[16 : 16+roleLen])
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	response := make([]byte, rolePolicyResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], rolePolicyResponseMagic)
	putBool(response[4:8], RoleIsWrite(role))
	putBool(response[8:12], RoleEnablesToolsByDefault(role))
	putBool(response[12:16], RoleResultCacheEnabled(role))
	putBool(response[16:20], RoleAutoToolsForInvocation(role, maxTurns, explicitTools))
	binary.LittleEndian.PutUint32(response[20:24], uint32(int32(RoleFinalAfterTurns(role))))
	putBool(response[24:28], RoleNeedsParentDiffEvidence(role))
	return response, bus.ModuleStatusOK
}
