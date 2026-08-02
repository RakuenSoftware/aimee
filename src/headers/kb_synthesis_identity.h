/* kb_synthesis_identity.h: mTLS identities for the kb -> aimee-llm hop. */
#ifndef KB_SYNTHESIS_IDENTITY_H
#define KB_SYNTHESIS_IDENTITY_H

/* Ensure $data_dir/synthesis-tls holds the identities for the synthesis sidecar:
 * a server certificate for |sidecar_host|, the kb's own client certificate, and the
 * CA both verify against. All are issued from the kb's existing CA.
 *
 * Idempotent: returns 0 without reissuing when the material is already present.
 * Re-issuing every boot would hand the sidecar a certificate its running peer does
 * not know about.
 *
 * Returns 0 on success, -1 on failure. Failure is not fatal to the kb -- synthesis
 * is optional -- but the sidecar will refuse to start without this material, which
 * is deliberate: it makes a provisioning failure loud at deploy rather than silent
 * until the first curation call. */
int kb_synthesis_identity_ensure(const char *data_dir, const char *sidecar_host);

#endif /* KB_SYNTHESIS_IDENTITY_H */
