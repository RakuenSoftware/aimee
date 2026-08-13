package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

type permReq struct{ w wireWriter }

func newPermReq(role string, defined bool) *permReq {
	r := &permReq{}
	r.w.u32(permissionsRequestMagic)
	r.w.u32(uint32(wireVersion))
	var flags uint32
	if defined {
		flags |= permFlagDefined
	}
	r.w.u32(flags)
	r.w.str(role)
	return r
}

func (r *permReq) grants(n int) *permReq {
	r.w.u32(uint32(n))
	return r
}

func (r *permReq) grant(name, enforcedAt string, scopes ...string) *permReq {
	r.w.str(name)
	r.w.str(enforcedAt)
	r.w.strings(scopes)
	return r
}

func (r *permReq) bytes() []byte { return r.w.buf }

type decodedGrant struct {
	enforcedAt string
	scopes     []string
}

func decodePermissions(t *testing.T, b []byte) (map[string]decodedGrant, []string) {
	t.Helper()
	r := &wireReader{buf: b}
	if r.u32() != permissionsResponseMagic {
		t.Fatalf("bad response magic")
	}
	out := map[string]decodedGrant{}
	for n := r.count(permissionsMaxGrants); n > 0; n-- {
		name := r.str()
		out[name] = decodedGrant{enforcedAt: r.str(), scopes: r.strings(permissionsMaxScopes)}
	}
	unenforced := r.strings(permissionsMaxGrants)
	if !r.done() {
		t.Fatalf("response has unread bytes")
	}
	return out, unenforced
}

func TestPermissionsStageAnswersFromTheBuiltInTable(t *testing.T) {
	response, status := handlePermissions(bus.ModuleInvocation{}, newPermReq("review", false).bytes())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	grants, unenforced := decodePermissions(t, response)

	if len(grants) != 1 {
		t.Fatalf("review holds tools and nothing else, got %v", grants)
	}
	if got := grants[PermTools]; got.enforcedAt != string(EnforceTools) {
		t.Errorf("tools enforced at %q, want %q", got.enforcedAt, EnforceTools)
	}
	if len(unenforced) != 0 {
		t.Errorf("a built-in role has nothing unenforced, got %v", unenforced)
	}
}

// Scopes and the operator's enforcement point survive the trip.
func TestPermissionsStageCarriesScopesAndPoints(t *testing.T) {
	request := newPermReq("custom", true).
		grants(2).
		grant(PermRepoWrite, "", "/srv/repo-a", "/srv/repo-b").
		grant("deploy", "deploy-gate", "staging").
		bytes()

	response, status := handlePermissions(bus.ModuleInvocation{}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	grants, unenforced := decodePermissions(t, response)

	write := grants[PermRepoWrite]
	if write.enforcedAt != string(EnforceMount) {
		t.Errorf("repo_write falls back to its built-in point, got %q", write.enforcedAt)
	}
	if len(write.scopes) != 2 || write.scopes[0] != "/srv/repo-a" {
		t.Errorf("scopes did not survive: %v", write.scopes)
	}

	deploy := grants["deploy"]
	if deploy.enforcedAt != "deploy-gate" {
		t.Errorf("the operator's point did not survive: %q", deploy.enforcedAt)
	}
	if len(unenforced) != 0 {
		t.Errorf("deploy names a point, so nothing is unenforced: %v", unenforced)
	}
}

// The answer the caller must not have to ask twice for.
func TestPermissionsStageReportsWhatNothingEnforces(t *testing.T) {
	request := newPermReq("custom", true).
		grants(2).
		grant(PermTools, "").
		grant("deploy", "").
		bytes()

	response, status := handlePermissions(bus.ModuleInvocation{}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	grants, unenforced := decodePermissions(t, response)

	if len(unenforced) != 1 || unenforced[0] != "deploy" {
		t.Fatalf("unenforced = %v, want [deploy]", unenforced)
	}
	if _, held := grants["deploy"]; !held {
		t.Error("it is still held; only its enforcement is missing")
	}
}

// A definition granting nothing is a powerless role. No definition falls back to
// the built-in. The wire has to keep those apart.
func TestAnEmptyDefinitionIsNotTheSameAsNoDefinition(t *testing.T) {
	empty, status := handlePermissions(bus.ModuleInvocation{},
		newPermReq("code", true).grants(0).bytes())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	if grants, _ := decodePermissions(t, empty); len(grants) != 0 {
		t.Errorf("an empty definition grants nothing, got %v", grants)
	}

	builtin, _ := handlePermissions(bus.ModuleInvocation{}, newPermReq("code", false).bytes())
	if grants, _ := decodePermissions(t, builtin); len(grants) == 0 {
		t.Error("no definition falls back to the built-in")
	}
}

func TestPermissionsStageRejectsMalformedRequests(t *testing.T) {
	good := newPermReq("review", false).bytes()

	badMagic := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], permissionsRequestMagic+1)

	badVersion := append([]byte(nil), good...)
	badVersion[4] = wireVersion + 1

	reservedSet := append([]byte(nil), good...)
	reservedSet[5] = 1

	unknownFlag := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(unknownFlag[8:12], permFlagsKnown|(1<<7))

	trailing := append(append([]byte(nil), good...), 0)

	hugeCount := newPermReq("review", true).bytes()
	hugeCount = append(hugeCount, 0xff, 0xff, 0xff, 0xff)

	for name, request := range map[string][]byte{
		"bad magic":      badMagic,
		"bad version":    badVersion,
		"reserved byte":  reservedSet,
		"unknown flag":   unknownFlag,
		"trailing bytes": trailing,
		"truncated":      good[:len(good)-1],
		"count ceiling":  hugeCount,
		"empty":          nil,
	} {
		if _, status := handlePermissions(bus.ModuleInvocation{}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: want InvalidRequest, got %v", name, status)
		}
	}
}

func TestPermissionsStageHonoursCancellation(t *testing.T) {
	request := newPermReq("review", false).bytes()
	if _, status := handlePermissions(bus.ModuleInvocation{DeadlineNS: 1}, request); status != bus.ModuleStatusCancelled {
		t.Errorf("want Cancelled, got %v", status)
	}
}
