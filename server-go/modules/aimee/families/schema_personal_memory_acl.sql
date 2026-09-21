-- Repair ACLs weakened by older PostgreSQL restart reconciliation. The store
-- bootstrap now preserves migration-owned ACLs; this migration repairs existing
-- personal history and progress without changing their contents or identities.
DO $personal_change_acl$
DECLARE recipient record; target record; role_name TEXT;
BEGIN
  FOR recipient IN SELECT DISTINCT acl.grantee,routine.proname FROM pg_proc AS routine,
    LATERAL aclexplode(routine.proacl) AS acl
    WHERE routine.oid IN ('user_memory_capture_change()'::regprocedure,
                          'user_memory_assign_revision()'::regprocedure,
                          'user_memory_retain_version()'::regprocedure)
      AND acl.grantee<>routine.proowner
  LOOP
    role_name := CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
      ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
    EXECUTE format('REVOKE ALL ON FUNCTION %I() FROM %s',recipient.proname,role_name);
  END LOOP;
  FOR target IN SELECT oid,relname,relowner FROM pg_class
    WHERE oid IN ('user_memory_collection_generation'::regclass,
                  'user_memory_invalidation_outbox'::regclass,
                  'user_memory_versions'::regclass)
  LOOP
    FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class AS relation,
      LATERAL aclexplode(relation.relacl) AS acl
      WHERE relation.oid=target.oid AND acl.grantee<>target.relowner
    LOOP
      role_name := CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
        ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
      EXECUTE format('REVOKE ALL ON TABLE %I FROM %s',target.relname,role_name);
      IF recipient.grantee<>0 THEN
        EXECUTE format('GRANT SELECT ON TABLE %I TO %s',target.relname,role_name);
      END IF;
    END LOOP;
  END LOOP;
END
$personal_change_acl$;
