DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimeetest') THEN
    CREATE ROLE aimeetest LOGIN SUPERUSER PASSWORD 'aimeetest';
  END IF;
END
$$;
