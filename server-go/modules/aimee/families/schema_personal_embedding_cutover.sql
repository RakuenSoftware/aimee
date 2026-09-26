-- The private index owner may stabilize source mutations for a short atomic
-- cutover without receiving direct access to protected journal/barrier tables.
DO $install_private_index_barrier$
DECLARE schema_name TEXT:=current_schema(); recipient RECORD; role_name TEXT;
BEGIN
 EXECUTE format($ddl$
 CREATE FUNCTION %1$I.memory_index_cutover_lock() RETURNS void
 LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I,pg_temp AS $body$
 BEGIN
  PERFORM id FROM memory_send_barrier WHERE id=1 FOR UPDATE NOWAIT;
  IF NOT FOUND THEN RAISE EXCEPTION 'private embedding cutover barrier unavailable'; END IF;
 END $body$;
 $ddl$,schema_name);
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc p,
  LATERAL aclexplode(p.proacl) acl WHERE p.oid='memory_send_guard_begin(text,integer)'::regprocedure
  AND acl.grantee<>p.proowner AND acl.grantee<>0
 LOOP
  role_name:=quote_ident(pg_get_userbyid(recipient.grantee));
  EXECUTE format('GRANT EXECUTE ON FUNCTION memory_index_cutover_lock() TO %s',role_name);
 END LOOP;
 REVOKE ALL ON FUNCTION memory_index_cutover_lock() FROM PUBLIC;
 IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
  GRANT EXECUTE ON FUNCTION memory_index_cutover_lock() TO aimee_store_runtime;
 END IF;
END $install_private_index_barrier$;
