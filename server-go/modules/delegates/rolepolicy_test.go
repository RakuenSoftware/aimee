package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// Ported from test_delegate_role.c.
func TestRoleIsWrite(t *testing.T) {
	for _, role := range []string{"code", "refactor", "implement", "build"} {
		if !RoleIsWrite(role) {
			t.Errorf("%q should be a write role", role)
		}
	}
	for _, role := range []string{"review", "validate", "summarize", "", "unknown"} {
		if RoleIsWrite(role) {
			t.Errorf("%q should not be a write role", role)
		}
	}
}

// A write role with tools off cannot fail visibly -- it returns a diff summary
// of code it never wrote -- so tools-on is the only honest default. That default
// IS the `tools` permission now, so this pins the permission and the aliases
// that resolve onto it.
func TestTheRolesThatHoldToolsByDefault(t *testing.T) {
	on := []string{"code", "refactor", "review", "search", "execute", "diagnose",
		"validate", "continuity", "beat-check"}
	for _, role := range on {
		if !RoleHasPermission(role, PermTools) {
			t.Errorf("%q should hold tools", role)
		}
	}
	off := []string{"summarize", "format", "draft", "explain", "reason", "plan", ""}
	for _, role := range off {
		if RoleHasPermission(role, PermTools) {
			t.Errorf("%q should not hold tools", role)
		}
	}
	// Aliases resolve before permissions are read.
	for _, alias := range []string{"implement", "reviewer", "inspect", "recall",
		"verifier", "research", "test", "check", "enforce", "build"} {
		if !RoleHasPermission(alias, PermTools) {
			t.Errorf("alias %q did not resolve to a role holding tools", alias)
		}
	}
}

// Caching is opt-in and only for pure text transforms: the same prompt against
// a changed working tree must never be answered from cache.
func TestRoleResultCacheEnabled(t *testing.T) {
	for _, role := range []string{"summarize", "format", "draft", "synthesize"} {
		if !RoleResultCacheEnabled(role) {
			t.Errorf("%q should be cacheable", role)
		}
	}
	for _, role := range []string{"code", "review", "search", "diagnose", "execute", "", "custom"} {
		if RoleResultCacheEnabled(role) {
			t.Errorf("%q must not be cacheable", role)
		}
	}
}

// The invocation's conditions, applied to the permission the caller resolved.
// The role is not consulted: a role an operator defined is visible only in that
// resolved set, and asking about the role here would answer from the built-in
// table and hand tools to a role defined without them.
func TestAutoToolsForInvocation(t *testing.T) {
	const holdsTools, holdsNone = true, false

	// An explicit request always wins, even for a one-turn probe.
	if !AutoToolsForInvocation(holdsNone, 1, true) {
		t.Error("explicit tools were ignored")
	}
	// One turn is a final-answer smoke probe, so no implicit tools.
	if AutoToolsForInvocation(holdsTools, 1, false) {
		t.Error("a one-turn run got implicit tools")
	}
	if !AutoToolsForInvocation(holdsTools, 8, false) {
		t.Error("a multi-turn run holding tools did not get them")
	}
	if AutoToolsForInvocation(holdsNone, 8, false) {
		t.Error("a run that does not hold tools was given them")
	}

	// The case the permission exists for: a role an operator defined without
	// tools does not get them, whatever the built-in table says about the name.
	definition, err := ParseRoleDefinition("permissions:\n  - knowledge_write\n")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	held := ResolveRolePermissions("code", definition).Has(PermTools)
	if AutoToolsForInvocation(held, 8, false) {
		t.Error("a defined role without tools was handed the built-in default")
	}
}

func TestRoleFinalAfterTurns(t *testing.T) {
	for role, want := range map[string]int{
		"validate": 8, "search": 10, "diagnose": 12,
		"code": -1, "review": -1, "": -1,
		// Aliases resolve first.
		"verifier": 8, "recall": 10, "inspect": 12,
	} {
		if got := RoleFinalAfterTurns(role); got != want {
			t.Errorf("%q = %d, want %d", role, got, want)
		}
	}
}

// continuity and beat-check survived the persona-vs-role cull: they are real
// read-only inspection actions a novel persona genuinely delegates, not
// restatements of who the delegate is. They inspect the world bible by default,
// but a one-turn probe still gets no tools.
func TestRoleNovelInspectionRoles(t *testing.T) {
	for _, role := range []string{"continuity", "beat-check"} {
		if RoleIsWrite(role) {
			t.Errorf("%q must not be a write role", role)
		}
	}
	for _, role := range []string{"continuity", "beat-check"} {
		if !RoleHasPermission(role, PermTools) {
			t.Errorf("%q inspects the world bible, so it holds tools", role)
		}
		if !AutoToolsForInvocation(RoleHasPermission(role, PermTools), 2, false) {
			t.Errorf("%q did not get its default tools", role)
		}
		if AutoToolsForInvocation(RoleHasPermission(role, PermTools), 1, false) {
			t.Errorf("a one-turn %q probe got implicit tools", role)
		}
	}
}

func rolePolicyRequestBytes(role string, maxTurns int, explicitTools bool) []byte {
	return rolePolicyRequestBytesHolding(role, maxTurns, explicitTools, false)
}

func rolePolicyRequestBytesHolding(role string, maxTurns int, explicitTools, holdsTools bool) []byte {
	request := make([]byte, rolePolicyRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], rolePolicyRequestMagic)
	request[4] = wireVersion
	if holdsTools {
		request[6] = 1
	}
	if explicitTools {
		request[5] = 1
	}
	binary.LittleEndian.PutUint32(request[8:12], uint32(int32(maxTurns)))
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(role)))
	copy(request[16:], role)
	return request
}

func TestRolePolicyStageRoundTrip(t *testing.T) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		rolePolicyRequestBytes("implement", 8, false))
	if status != bus.ModuleStatusOK || len(response) != rolePolicyResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != rolePolicyResponseMagic {
		t.Fatalf("status %d, %d bytes", status, len(response))
	}
	// "implement" is an alias for the write role "code".
	if binary.LittleEndian.Uint32(response[4:8]) != 1 {
		t.Error("alias did not resolve to a write role")
	}
	if binary.LittleEndian.Uint32(response[12:16]) != 0 {
		t.Error("a write role was reported cacheable")
	}
	if int32(binary.LittleEndian.Uint32(response[20:24])) != -1 {
		t.Error("code should have no early-final turn")
	}

	// diagnose has one, and a one-turn probe still gets no implicit tools.
	response, _ = Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		rolePolicyRequestBytes("diagnose", 1, false))
	if int32(binary.LittleEndian.Uint32(response[20:24])) != 12 {
		t.Error("diagnose lost its early-final turn")
	}
	if binary.LittleEndian.Uint32(response[16:20]) != 0 {
		t.Error("a one-turn probe got implicit tools")
	}
}

// The auto-tools answer comes from the byte the caller sent, not from the role.
//
// This is the whole point of carrying the permission: `code` ships with tools,
// so a stage that asked about the role would say yes here no matter what the
// caller resolved.
func TestTheStageAnswersAutoToolsFromTheCarriedPermission(t *testing.T) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		rolePolicyRequestBytesHolding("code", 8, false, false))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	if binary.LittleEndian.Uint32(response[16:20]) != 0 {
		t.Error("a run that does not hold tools was given them")
	}

	response, _ = Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		rolePolicyRequestBytesHolding("code", 8, false, true))
	if binary.LittleEndian.Uint32(response[16:20]) == 0 {
		t.Error("a run holding tools did not get them")
	}

	// And the role field is still what the other answers are computed from.
	if binary.LittleEndian.Uint32(response[4:8]) != 1 {
		t.Error("code is still a write role")
	}
}

func TestRolePolicyStageRejectsInvalidEnvelope(t *testing.T) {
	good := rolePolicyRequestBytes("code", 4, false)

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		good[:rolePolicyRequestLen-1]); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("truncated status = %d", status)
	}

	lying := rolePolicyRequestBytes("code", 4, false)
	binary.LittleEndian.PutUint32(lying[12:16], uint32(roleMax+1))
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRolePolicy}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("over-long role status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRolePolicy, DeadlineNS: 1},
		good); status != bus.ModuleStatusCancelled {
		t.Errorf("expired status = %d", status)
	}
}

// The parent-diff evidence roles are the read-only INSPECTION ones: they run
// against an isolated checkout whose diff is not the one they were asked about.
func TestRoleNeedsParentDiffEvidence(t *testing.T) {
	for _, role := range []string{"validate", "review", "diagnose", "test"} {
		if !RoleNeedsParentDiffEvidence(role) {
			t.Errorf("%q inspects; it needs the parent diff", role)
		}
	}
	// A write role is PRODUCING the diff, and a role with no inspection duty has
	// nothing to ground.
	for _, role := range []string{"code", "refactor", "explain", "summarize", ""} {
		if RoleNeedsParentDiffEvidence(role) {
			t.Errorf("%q should not be handed the parent diff", role)
		}
	}
	// Aliases resolve first, so "reviewer" cannot get a different answer from
	// "review".
	if RoleNeedsParentDiffEvidence("review") != RoleNeedsParentDiffEvidence("reviewer") {
		t.Error("an alias must not change the answer")
	}
}

func TestRolePolicyStageCarriesParentDiffEvidence(t *testing.T) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		rolePolicyRequestBytes("review", -1, false))
	if status != bus.ModuleStatusOK || len(response) != rolePolicyResponseLen {
		t.Fatalf("status %v len %d", status, len(response))
	}
	if binary.LittleEndian.Uint32(response[24:28]) == 0 {
		t.Error("review needs the parent diff and the wire should say so")
	}

	response, status = Handle(bus.ModuleInvocation{StageID: StageRolePolicy},
		rolePolicyRequestBytes("code", -1, false))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	if binary.LittleEndian.Uint32(response[24:28]) != 0 {
		t.Error("a write role must not be handed the parent diff")
	}
}

// Which roles may mutate what aimee knows.
//
// This was RoleSeesCurrentCodeOnly, phrased the other way round and answered by
// its own list. It is now the `knowledge_write` permission, and these are the
// same answers: the equivalence was proved role by role against the predicate
// in the commit that introduced the permission, before it was deleted.
func TestWhichRolesMayMutateWhatAimeeKnows(t *testing.T) {
	for _, role := range []string{"review", "diagnose", "inspect"} {
		if RoleHasPermission(role, PermKnowledgeWrite) {
			t.Errorf("%q answers about the code as it is now", role)
		}
	}
	for _, role := range []string{"code", "validate", "execute", "search", "plan"} {
		if !RoleHasPermission(role, PermKnowledgeWrite) {
			t.Errorf("%q should keep its index and memory tools", role)
		}
	}
}

// An alias holds exactly what the name it resolves to holds.
//
// This replaces a test that pinned the opposite. The C compared the raw role, so
// `--role reviewer` kept the index, memory, docs, notes and remote MCP tools
// that `--role review` was denied: the same delegate, doing the same job, with a
// different tool surface depending on which spelling the caller typed.
func TestAnAliasIsConfinedLikeTheRoleItResolvesTo(t *testing.T) {
	for _, pair := range [][2]string{
		{"reviewer", "review"},
		{"inspect", "diagnose"},
		{"verifier", "validate"},
		{"implement", "code"},
	} {
		alias, canonical := pair[0], pair[1]
		if RoleHasPermission(alias, PermKnowledgeWrite) != RoleHasPermission(canonical, PermKnowledgeWrite) {
			t.Errorf("%q and %q must agree: an alias is the same role", alias, canonical)
		}
	}
	// The change this test exists for.
	if RoleHasPermission("reviewer", PermKnowledgeWrite) {
		t.Error("reviewer resolves to review, and review is confined")
	}
	// And what did NOT change: validate and its aliases keep their tools.
	if !RoleHasPermission("verifier", PermKnowledgeWrite) ||
		!RoleHasPermission("validate", PermKnowledgeWrite) {
		t.Error("validate is not confined, so neither is its alias")
	}
}
