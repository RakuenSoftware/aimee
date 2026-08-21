CREATE SCHEMA IF NOT EXISTS aimee_test_seed;

-- Snapshot every public table that carries rows right after the schema is
-- applied. Those rows ARE the baseline a test expects to find (kind_lifecycle,
-- tool_registry, kb_meta and friends are seeded by schema.sql), so the per-test
-- reset has to put them back after truncating rather than leave the table bare.
CREATE OR REPLACE FUNCTION aimee_test_seed_capture() RETURNS int AS $$
DECLARE
   r        record;
   nonempty boolean;
   n        int := 0;
BEGIN
   FOR r IN SELECT tablename AS t FROM pg_tables WHERE schemaname = 'public' LOOP
      EXECUTE format('SELECT EXISTS (SELECT 1 FROM public.%I)', r.t) INTO nonempty;
      IF NOT nonempty THEN
         CONTINUE;
      END IF;
      EXECUTE format('DROP TABLE IF EXISTS aimee_test_seed.%I', r.t);
      EXECUTE format('CREATE TABLE aimee_test_seed.%I AS SELECT * FROM public.%I', r.t, r.t);
      n := n + 1;
   END LOOP;
   RETURN n;
END $$ LANGUAGE plpgsql;

-- Return the database to the freshly-applied-schema state.
--
-- Only non-empty tables are touched: TRUNCATE takes an ACCESS EXCLUSIVE lock and
-- does file work per relation, so blanket-truncating all 217 costs ~250 ms while
-- a typical test dirties a handful and resets in single-digit ms.
--
-- session_replication_role = replica suppresses user triggers for the duration.
-- The schema installs db3_reject_vector_truncate on the eight vector tables to
-- stop a production operator truncating an index out from under its rebuild;
-- that guard is correct and stays, but a test fixture resetting its own scratch
-- database is exactly the case it is not aimed at.
CREATE OR REPLACE FUNCTION aimee_test_reset() RETURNS void AS $$
DECLARE
   r        record;
   nonempty boolean;
   victims  text[] := '{}';
BEGIN
   PERFORM set_config('session_replication_role', 'replica', true);

   FOR r IN SELECT tablename AS t FROM pg_tables WHERE schemaname = 'public' LOOP
      EXECUTE format('SELECT EXISTS (SELECT 1 FROM public.%I)', r.t) INTO nonempty;
      IF nonempty THEN
         victims := victims || format('public.%I', r.t);
      END IF;
   END LOOP;

   IF array_length(victims, 1) > 0 THEN
      EXECUTE format('TRUNCATE %s RESTART IDENTITY CASCADE', array_to_string(victims, ', '));
   END IF;

   FOR r IN SELECT tablename AS t FROM pg_tables WHERE schemaname = 'aimee_test_seed' LOOP
      EXECUTE format('INSERT INTO public.%I SELECT * FROM aimee_test_seed.%I', r.t, r.t);
   END LOOP;

   PERFORM set_config('session_replication_role', 'origin', true);
END $$ LANGUAGE plpgsql;
