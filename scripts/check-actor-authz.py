#!/usr/bin/env python3
"""Gate: per-actor authorization survives the shared-environment model.

The deployment collapsed several PER-ACTOR NAMESPACES into one environment: one
credential vault, one workspace root, one runtime dir, one Claude-tab binding.
That is deliberate (see the tenancy note in modules/vault/vault_service.h).

Collapsing a namespace is not the same as dropping an authorization decision,
and the migration confused the two in four places. Each removed a check while
leaving the surrounding code still promising it:

  * turn_registry_cancel stopped comparing the caller's attested principal to
    the session owner, while handle_chat_graceful_cancel kept passing the
    principal and kept rendering `rc < 0` as "forbidden: not the session owner".
    Any attested caller could cancel any session.
  * wf_owns became `return 1`, while the route table kept admitting every
    workflow lifecycle mutation on CAP_DASHBOARD_READ *because* the handler
    re-checked ownership. Any dashboard reader could stop or delete another
    actor's work item.
  * vault_service_rekey_password returned VAULT_OK for a principal with no
    vault. The old password is verified AFTER that point, so the route answered
    {"status":"ok"} to a password change that verified nothing.
  * handle_vault_unlock initialised its status to VAULT_OK and skipped the
    unlock when no legacy vault existed, so /v1/vault/unlock succeeded for any
    password.

None of those had to happen for the namespaces to merge. This check pins the
repaired sites so the next migration pass cannot quietly vacate them again: a
namespace may be shared, an authorization decision may not be skipped.

Each rule is a REQUIRED pattern (the check must still be there) or a FORBIDDEN
one (the vacated form must not come back), anchored to a named function so a
rename surfaces here rather than silently passing.

  --plant-test  prove the gate is not vacuously passing.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# (file, function, kind, pattern, why)
#   kind "require": the pattern MUST appear inside the function.
#   kind "forbid":  the pattern must NOT appear inside the function.
RULES = [
    ("server/turn_registry.c", "turn_registry_cancel", "require",
     r"presence_session_owner\s*\(",
     "cancel must compare the caller's attested principal to the session owner"),
    ("server/turn_registry.c", "turn_registry_cancel", "require",
     r"strcmp\s*\(\s*owner\s*,\s*owner_principal\s*\)\s*!=\s*0",
     "the owner comparison itself must remain, not just the lookup"),
    ("server/turn_registry.c", "turn_registry_cancel", "forbid",
     r"\(void\)\s*owner_principal\s*;",
     "discarding owner_principal is how the cross-principal cancel check was lost"),

    ("server/server_workflow_api.c", "wf_owns", "require",
     r"server_http_identity_principal\s*\(",
     "work-item ownership must be decided against the calling principal"),
    ("server/server_workflow_api.c", "wf_owns", "forbid",
     r"\(void\)\s*wi\s*;",
     "an unconditional wf_owns opens every workflow route to any dashboard reader"),
    ("server/server_workflow_api.c", "wf_load_for_mutation", "require",
     r"!is_operator\s*&&\s*!wf_owns\s*\(",
     "lifecycle mutations must 403 a non-owner non-operator"),

    ("modules/vault/vault_service.c", "vault_service_rekey_password", "require",
     r"vault_store_salt_readonly\s*\([^)]*\)\s*!=\s*0\s*\)\s*\n\s*return\s+VAULT_ERR_CRYPTO\s*;",
     "a rekey against a principal with no vault must fail closed, never report success"),

    ("server/server_vault.c", "handle_vault_unlock", "forbid",
     r"vault_status_t\s+st\s*=\s*VAULT_OK\s*;",
     "seeding the unlock status with a success value makes any skipped path authenticate nobody"),
    ("server/server_vault.c", "handle_vault_unlock", "require",
     r"return\s+vault_send_status_error\s*\(\s*conn\s*,\s*VAULT_NO_ENTRY\s*\)\s*;",
     "no legacy vault must return before the success tail, not fall through to it"),
]


def function_body(text, name):
    """Source of `name` from its definition to the closing brace in column 0."""
    m = re.search(rf"^[A-Za-z_][\w \*]*\b{re.escape(name)}\s*\(", text, re.M)
    if not m:
        return None
    start = m.start()
    end = text.find("\n}", start)
    return text[start:end + 2] if end != -1 else text[start:]


def check(mutate=None):
    failures = []
    for rel, func, kind, pattern, why in RULES:
        path = SRC / rel
        if not path.exists():
            failures.append(f"{rel}: file is gone; {func} carried an authorization check")
            continue
        text = path.read_text(encoding="utf-8")
        if mutate and mutate[0] == rel and mutate[1] == func:
            text = mutate[2](text)
        body = function_body(text, func)
        if body is None:
            failures.append(f"{rel}: {func} not found (renamed?); it carried: {why}")
            continue
        found = re.search(pattern, body) is not None
        if kind == "require" and not found:
            failures.append(f"{rel}: {func} no longer has the check — {why}")
        elif kind == "forbid" and found:
            failures.append(f"{rel}: {func} reintroduced the vacated form — {why}")
    return failures


def main():
    if "--plant-test" in sys.argv:
        # Vacate wf_owns exactly the way the migration did and confirm we catch it.
        planted = check(mutate=("server/server_workflow_api.c", "wf_owns",
                                lambda t: t.replace("server_http_identity_principal()",
                                                    "NULL /* planted */")))
        if not any("wf_owns" in f for f in planted):
            print("check-actor-authz: FAIL — plant-test did not catch a vacated check")
            return 1
        print("check-actor-authz: plant-test ok (vacated authorization check detected)")
        return 0

    failures = check()
    if failures:
        print(f"check-actor-authz: FAIL — {len(failures)} per-actor authorization "
              f"check(s) missing:")
        for f in failures:
            print(f"  {f}")
        print("  A shared namespace does not imply a shared authority. If a check here is "
              "genuinely obsolete, move the decision to a named enforcement point and "
              "update this file in the same commit.")
        return 1
    print(f"check-actor-authz: ok ({len(RULES)} per-actor authorization checks intact)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
