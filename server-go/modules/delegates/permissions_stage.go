package delegates

import "github.com/JBailes/aimee/server-go/bus"

// Resolving what a delegate may do, once.
//
// The caller sends the role and, when the operator wrote one, the role template
// frontmatter verbatim. What comes back is the resolved set: the permissions,
// their scopes, where each is enforced, and which of them nothing is enforcing.
//
// The frontmatter arrives as TEXT and is parsed here. What a permission block
// means is a rule; the caller's job is to hand over the bytes it found on disk.
// A definition it cannot read is refused outright rather than partly applied --
// a permission silently dropped is a power an operator believes they granted.
//
// Length-prefixed, because permission and scope names are prose an operator
// wrote. Every read is bounds-checked and every count capped: a request read
// differently from how it was written would resolve a different set of
// permissions than the caller asked about, and grant or deny the wrong things.

const (
	StagePermissions uint32 = 15
	EventPermissions uint32 = 6671

	permissionsRequestMagic  uint32 = 0x51524550 /* "PERQ" */
	permissionsResponseMagic uint32 = 0x53524550 /* "PERS" */

	permissionsMaxScopes = 256

	// A role template is capped at 4096 bytes on disk (ROLE_TEMPLATE_MAX_SIZE);
	// this is room for that and no more.
	permissionsMaxDefinition = 8192

	// permFlagDefined says role template frontmatter follows. Absent, the
	// built-in table answers, and the two are not the same: a definition
	// granting nothing is a deliberate powerless role, while no definition at
	// all falls back to what ships.
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

	var frontmatter string
	if flags&permFlagDefined != 0 {
		frontmatter = r.str()
		if len(frontmatter) > permissionsMaxDefinition {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}

	if !r.done() {
		return nil, bus.ModuleStatusInvalidRequest
	}

	// An unreadable definition is a refusal, not a fallback. Falling back to the
	// built-in table would hand a delegate the powers that role ships with while
	// the operator believes it holds the ones they wrote.
	defined, err := ParseRoleDefinition(frontmatter)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	permissions := ResolveRolePermissions(role, defined)

	// The unenforced list and the denied tools are sent whether or not either is
	// empty. A caller that has to ask a second question to find out it was handed
	// a permission nobody evaluates, or which tools its set withholds, will
	// eventually not ask.
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
	w.strings(DeniedTools(permissions))
	return w.buf, bus.ModuleStatusOK
}
