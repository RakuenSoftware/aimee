package delegates

import "github.com/JBailes/aimee/server-go/bus"

// Resolving what a delegate may do, once.
//
// The caller sends the role and, when the operator has defined one, the role
// definition that came with it. What comes back is the resolved set: the
// permissions, their scopes, where each is enforced, and which of them nothing
// is enforcing.
//
// Length-prefixed, because permission and scope names are prose an operator
// wrote. Every read is bounds-checked and every count capped: a request read
// differently from how it was written would resolve a different set of
// permissions than the caller asked about, and grant or deny the wrong things.

const (
	StagePermissions uint32 = 22
	EventPermissions uint32 = 6678

	permissionsRequestMagic  uint32 = 0x51524550 /* "PERQ" */
	permissionsResponseMagic uint32 = 0x53524550 /* "PERS" */

	permissionsMaxGrants = 256
	permissionsMaxScopes = 256

	// permFlagDefined says a role definition follows. Absent, the built-in
	// table answers, and the two are not the same: a definition granting
	// nothing is a deliberate powerless role, while no definition at all falls
	// back to what ships.
	permFlagDefined uint32 = 1 << 0

	permFlagsKnown = permFlagDefined
)

func handlePermissions(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	r := &wireReader{buf: request}
	if r.u32() != permissionsRequestMagic || r.u32() != uint32(wireVersion) {
		return nil, bus.ModuleStatusInvalidRequest
	}

	flags := r.u32()
	if flags&^permFlagsKnown != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	role := r.str()

	var defined *RoleDefinition
	if flags&permFlagDefined != 0 {
		definition := RoleDefinition{}
		count := r.count(permissionsMaxGrants)
		for i := 0; i < count && !r.bad; i++ {
			grant := Grant{Name: r.str()}
			grant.EnforcedAt = EnforcementPoint(r.str())
			grant.Scopes = r.strings(permissionsMaxScopes)
			definition.Grants = append(definition.Grants, grant)
		}
		defined = &definition
	}

	if !r.done() {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	permissions := ResolveRolePermissions(role, defined)

	// The unenforced list is sent whether or not it is empty. A caller that has
	// to ask a second question to find out it was handed a permission nobody
	// evaluates will eventually not ask.
	names := permissions.Names()
	unenforced := permissions.Unenforced()

	w := &wireWriter{}
	w.u32(permissionsResponseMagic)
	w.u32(uint32(len(names)))
	for _, name := range names {
		w.str(name)
		w.str(string(permissions.EnforcedAt(name)))
		w.strings(permissions.Scopes(name))
	}
	w.strings(unenforced)
	return w.buf, bus.ModuleStatusOK
}
