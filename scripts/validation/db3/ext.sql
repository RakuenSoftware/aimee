CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_trgm;
SELECT string_agg(extname || ' ' || extversion, ', ' ORDER BY extname) AS extensions
  FROM pg_extension;
