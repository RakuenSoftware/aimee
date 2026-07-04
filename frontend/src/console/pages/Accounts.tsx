// S0 placeholder. S3 fills this with enroll-a-client, the enrollments table +
// revoke, the scope-lattice view, and the OIDC config editor (backends land in
// S2a / S2b).
export default function Accounts() {
  return (
    <section>
      <h2>Accounts</h2>
      <p>Client enrollment, certificate revocation, scopes, and OIDC config land in S2/S3.</p>
    </section>
  );
}
