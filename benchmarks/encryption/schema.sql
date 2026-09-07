-- Disposable benchmark database only. Keys and content are synthetic fixtures.
CREATE EXTENSION pgcrypto;
CREATE EXTENSION pg_trgm;

CREATE UNLOGGED TABLE fixture (
    id bigint PRIMARY KEY, project integer, body text, grams text[]
);
CREATE TABLE plain (
    id bigint PRIMARY KEY, project integer NOT NULL, body text NOT NULL,
    lexical tsvector NOT NULL
);
CREATE TABLE encrypted (
    id bigint PRIMARY KEY, project integer NOT NULL, ciphertext bytea NOT NULL,
    grams text[] NOT NULL, lexical tsvector NOT NULL
);

-- Public deterministic fixture keys: these are not production keys or a Vault substitute.
CREATE FUNCTION fixture_key(bigint) RETURNS text
LANGUAGE sql IMMUTABLE STRICT AS $$
    SELECT encode(digest('aimee-encryption-benchmark:' || $1::text, 'sha256'), 'hex')
$$;

-- Exercise PGP decryption plus a record-context check. The full proposed Vault
-- scope-wrap/authorization protocol is outside this database benchmark.
CREATE FUNCTION read_body(bytea, text, bigint) RETURNS text
LANGUAGE plpgsql STABLE STRICT COST 1000 AS $$
DECLARE raw bytea;
BEGIN
    raw := pgp_sym_decrypt_bytea($1, $2);
    IF substring(raw FROM 1 FOR 8) <> int8send($3) THEN
        RAISE EXCEPTION 'benchmark record context mismatch';
    END IF;
    RETURN convert_from(substring(raw FROM 9), 'UTF8');
END
$$;
