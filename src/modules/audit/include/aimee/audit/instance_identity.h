/* instance_identity.h: which aimee daemon an instance is, and the role
 * identities its ring must refuse.
 *
 * aimee composes one server instance and one KB instance. Each runs its own
 * ouroboros ring, and each has a reserved first-party role identity that may
 * attach only to its own instance. The ring knows nothing of roles: it is
 * handed, per instance, the role refs to refuse (aimee_instance_refused_role_refs). */
#ifndef AIMEE_AUDIT_INSTANCE_IDENTITY_H
#define AIMEE_AUDIT_INSTANCE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/* Reserved first-party composition identities (principal class
 * OURO_RUNTIME_ROLE_CLASS). Admission, not a mutable module setting, decides
 * which role may attach to this instance. */
#define AIMEE_SERVER_ROLE_REF 34U
#define AIMEE_KB_ROLE_REF     35U

typedef enum
{
   AIMEE_INSTANCE_UNSET = 0,
   AIMEE_INSTANCE_SERVER = 1,
   AIMEE_INSTANCE_KB = 2
} aimee_instance_role_t;

/* Validate the Go-owned first-boot latch, invoking the installed role's Go
 * bootstrap only when it is absent. runtime_binary is an optional native-build
 * path to the same module runtime; NULL uses the installed role executable. */
int aimee_instance_ensure_identity(const char *home, aimee_instance_role_t role,
                                   const char *runtime_binary);

/* The role refs an instance of this role refuses: the other role's, or both
 * for an instance with no role. Static storage; never NULL. */
const uint32_t *aimee_instance_refused_role_refs(aimee_instance_role_t role, size_t *count);

#endif
