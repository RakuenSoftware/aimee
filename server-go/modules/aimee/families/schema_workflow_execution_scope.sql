-- Opaque host-issued execution authorization. No user principal/token material.
ALTER TABLE lifecycle_work_item ADD COLUMN execution_scope TEXT NOT NULL DEFAULT ''
 CHECK (execution_scope = '' OR execution_scope ~ '^[0-9a-f]{64}$');
CREATE FUNCTION lifecycle_work_item_scope_immutable() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
 IF NEW.execution_scope IS DISTINCT FROM OLD.execution_scope THEN
  RAISE EXCEPTION 'workflow execution scope is immutable';
 END IF;
 RETURN NEW;
END;
$$;
CREATE TRIGGER lifecycle_work_item_scope_immutable BEFORE UPDATE OF execution_scope
 ON lifecycle_work_item FOR EACH ROW EXECUTE FUNCTION lifecycle_work_item_scope_immutable();
