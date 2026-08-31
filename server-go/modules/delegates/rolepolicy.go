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
	rolePolicyResponseLen          = 32
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

// RoleIsBuiltIn reports whether a role is one that ships.
//
// It reads the permission table, which IS the list of shipped roles: a role with
// no entry there holds nothing, so treating it as known would mean dispatching a
// delegate that can do nothing and saying nothing about why.
//
// C keeps the other half of the question -- whether a template file defines a
// custom role -- because that is a filesystem lookup, not a rule.
func RoleIsBuiltIn(role string) bool {
	_, ok := builtinRolePermissions[canonicalRole(role)]
	return ok
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

// TaskShape says what KIND of work a role does. It shapes how context is
// assembled and which opening instruction the run gets. It is not a permission
// and it grants nothing.
//
// The values match task_type_t in src/headers/agent_types.h.
type TaskShape uint32

const (
	TaskGeneral TaskShape = iota
	TaskBugFix
	TaskRefactor
	TaskFeature
	TaskReview
	TaskTest
)

// RoleTaskShape maps a role onto the shape of work it does.
//
// This replaces a keyword scan of the brief. That scan read the prose at every
// context refresh, so a long review whose prompt said "must be fixed" was
// reclassified as a bug fix mid-run and handed execution-agent instructions
// partway through the job it was doing correctly. The role is stated once by
// the caller and does not change while the run is in flight.
//
// A role nobody mapped is general, which is the same neutral weighting the
// keyword scan fell back to when it recognised nothing.
func RoleTaskShape(role string) TaskShape {
	switch canonicalRole(role) {
	case "review":
		return TaskReview
	case "diagnose":
		return TaskBugFix
	case "refactor":
		return TaskRefactor
	case "code":
		return TaskFeature
	case "validate", "test":
		return TaskTest
	}
	return TaskGeneral
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

// AutoToolsForInvocation applies one invocation's conditions to the `tools`
// permission the caller already resolved.
//
// It takes holdsTools rather than a role on purpose. The permission is resolved
// once when the delegate is created, and a role an operator defined is only
// visible in that resolved set -- asking about the role again here would answer
// from the built-in table and hand tools to a role defined without them.
//
// A single-turn run is a final-answer smoke probe, so it gets no implicit
// tools; asking for them explicitly still wins.
func AutoToolsForInvocation(holdsTools bool, maxTurns int, explicitTools bool) bool {
	if explicitTools {
		return true
	}
	if maxTurns == 1 {
		return false
	}
	return holdsTools
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
		request[4] != wireVersion || request[5] > 1 || request[6] > 1 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	explicitTools := request[5] == 1
	holdsTools := request[6] == 1
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
	putBool(response[8:12], RoleIsBuiltIn(role))
	putBool(response[12:16], RoleResultCacheEnabled(role))
	putBool(response[16:20], AutoToolsForInvocation(holdsTools, maxTurns, explicitTools))
	binary.LittleEndian.PutUint32(response[20:24], uint32(int32(RoleFinalAfterTurns(role))))
	putBool(response[24:28], RoleNeedsParentDiffEvidence(role))
	binary.LittleEndian.PutUint32(response[28:32], uint32(RoleTaskShape(role)))
	return response, bus.ModuleStatusOK
}
