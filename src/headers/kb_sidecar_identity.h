/* kb_sidecar_identity.h: mTLS identities for the kb's outbound sidecar hops. */
#ifndef KB_SIDECAR_IDENTITY_H
#define KB_SIDECAR_IDENTITY_H

/* Ensure $data_dir/$subdir holds the identities for one sidecar hop: a server
 * certificate for |sidecar_host|, a client certificate for the kb named |client_cn|,
 * and the CA both verify against. All are issued from the kb's existing CA.
 *
 * Idempotent: returns 0 without reissuing when the material is already present.
 * Re-issuing every boot would hand the sidecar a certificate its running peer does
 * not know about.
 *
 * Returns 0 on success, -1 on failure. Failure is not fatal to the kb, since every
 * sidecar is optional, but the sidecar will refuse to start without this material,
 * which is deliberate: it makes a provisioning failure loud at deploy rather than
 * silent until the first call. */
int kb_sidecar_identity_ensure(const char *data_dir, const char *subdir,
                               const char *sidecar_host, const char *client_cn);

/* The synthesis hop: kb -> aimee-llm, material in $data_dir/synthesis-tls. */
int kb_synthesis_identity_ensure(const char *data_dir, const char *sidecar_host);

/* The embedder hop: kb -> aimee-embedder-{a25m,nomic}, material in
 * $data_dir/embedder-tls. Independent of the synthesis hop by design; see the
 * comment on the definition. */
int kb_embedder_identity_ensure(const char *data_dir, const char *sidecar_host);

#endif /* KB_SIDECAR_IDENTITY_H */
