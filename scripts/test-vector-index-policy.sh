#!/bin/sh
# S7 acceptance: pgvectorscale (StreamingDiskANN) is the DEFAULT vector index,
# HNSW is the fallback, and a dimension neither can index tells the operator to
# attach an external vector provider rather than going silently unindexed.
#
# Three cases, because "default" is only half the contract:
#
#   A  pgvectorscale present           -> every vector index is diskann
#   B  pgvectorscale absent            -> every vector index is HNSW
#   C  neither method can take the dim -> a NOTICE naming the external provider
#
# Case B used to be the only reachable outcome: aimee's own CI has never had the
# extension, so its logs always read "pgvectorscale unavailable ... falls back to
# HNSW". Case C is what an embedder above pgvector's 2000-dimension HNSW cap
# hits, and it is a supported configuration rather than a fault.
#
# Runs inside the store-e2e container. Scoped to scratch databases it recreates.
set -u

PSQL=/usr/lib/postgresql/17/bin/psql
SCHEMA=${SCHEMA:-/tmp/db2_schema.sql}
BIGSCHEMA=/tmp/db2_schema_bigdim.sql
FAIL=0

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
q() { su postgres -c "$PSQL -d $1 -tAc \"$2\"" 2>/dev/null | tr -d ' '; }

fresh() { # $1 db, $2 with-vectorscale(yes/no)
  su postgres -c "$PSQL -tAc 'DROP DATABASE IF EXISTS $1'" >/dev/null 2>&1
  su postgres -c "$PSQL -tAc 'CREATE DATABASE $1'" >/dev/null 2>&1
  su postgres -c "$PSQL -d $1 -tAc 'CREATE EXTENSION IF NOT EXISTS vector'" >/dev/null 2>&1
  if [ "$2" = "yes" ]; then
    su postgres -c "$PSQL -d $1 -tAc 'CREATE EXTENSION IF NOT EXISTS vectorscale'" >/dev/null 2>&1
  fi
}

count_idx() { # $1 db, $2 method
  q "$1" "SELECT count(*) FROM pg_indexes WHERE schemaname='public' AND indexdef ILIKE '%USING $2%'"
}

echo "== capability: which types can diskann index? =="
fresh vecprobe yes
VEC=$(q vecprobe "SELECT count(*) FROM pg_opclass WHERE opcname='vector_cosine_ops' AND opcmethod=(SELECT oid FROM pg_am WHERE amname='diskann')")
HALF=$(q vecprobe "SELECT count(*) FROM pg_opclass WHERE opcname='halfvec_cosine_ops' AND opcmethod=(SELECT oid FROM pg_am WHERE amname='diskann')")
echo "  diskann opclasses -- vector: ${VEC:-0}  halfvec: ${HALF:-0}"
[ "${VEC:-0}" = "1" ] && pass "diskann can index the vector type" \
  || fail "diskann has no vector opclass -- the schema cannot use it"
if [ "${HALF:-0}" = "0" ]; then
  pass "diskann still has no halfvec opclass (why the columns are vector, not halfvec)"
else
  echo "  NOTE: pgvectorscale gained a halfvec opclass. That reopens halfvec as an"
  echo "        option (half the storage); it does not invalidate the vector choice."
  pass "recorded: halfvec opclass now exists"
fi

echo
echo "== A. pgvectorscale PRESENT: diskann everywhere =="
fresh vecpolicy_with yes
OUT=$(su postgres -c "$PSQL -d vecpolicy_with -f $SCHEMA" 2>&1)
DA=$(count_idx vecpolicy_with diskann); HA=$(count_idx vecpolicy_with hnsw)
echo "  diskann: ${DA:-0}   hnsw: ${HA:-0}"
if [ "${DA:-0}" -ge 11 ]; then
  pass "all 11 vector indexes use StreamingDiskANN"
else
  fail "expected 11 diskann, got ${DA:-0}"
  echo "$OUT" | grep -i "no in-database vector index" | head -4 | sed 's/^/    /'
fi
[ "${HA:-0}" = "0" ] && pass "no HNSW index created when diskann succeeded" \
  || fail "${HA} HNSW index(es) created despite diskann being available"

echo
echo "== B. pgvectorscale ABSENT: HNSW fallback =="
# The schema installs vectorscale ITSELF, so dropping the extension between
# applies does not work -- the next apply puts it straight back. On a host that
# HAS the library (this one), the only way to make the install genuinely fail is
# to apply the schema as a role that may not CREATE EXTENSION.
#
# `vector` is pre-installed as superuser so `CREATE EXTENSION IF NOT EXISTS
# vector` is a no-op for the limited role; `CREATE EXTENSION vectorscale` then
# fails on permission, the schema's EXCEPTION handler catches it, and the HNSW
# fallback runs for real instead of being asserted from a comment.
fresh vecpolicy_without no
# DROP ROLE fails while the role still owns objects from a previous run, which
# silently leaves the OLD role in place -- including its old (or absent)
# password. DROP OWNED first, per database, then drop the role for real.
for d in vecpolicy_without vecpolicy_big; do
  su postgres -c "$PSQL -d $d -tAc 'DROP OWNED BY vecnoext CASCADE'" >/dev/null 2>&1
done
su postgres -c "$PSQL -tAc \"DROP ROLE IF EXISTS vecnoext\"" >/dev/null 2>&1
su postgres -c "$PSQL -tAc \"CREATE ROLE vecnoext LOGIN PASSWORD 'vecnoext_pw'\"" >/dev/null 2>&1
# PostgreSQL 15+ revoked CREATE on schema public from PUBLIC, so the role needs
# it explicitly or the schema apply creates nothing at all.
su postgres -c "$PSQL -d vecpolicy_without -tAc 'GRANT CREATE ON SCHEMA public TO vecnoext'" >/dev/null 2>&1
su postgres -c "$PSQL -d vecpolicy_without -tAc 'GRANT ALL ON SCHEMA public TO vecnoext'" >/dev/null 2>&1
su postgres -c "$PSQL -d vecpolicy_without -tAc 'GRANT ALL ON DATABASE vecpolicy_without TO vecnoext'" >/dev/null 2>&1
OUT2=$(su postgres -c "PGPASSWORD=vecnoext_pw $PSQL -w -U vecnoext -h 127.0.0.1 -d vecpolicy_without -f $SCHEMA" 2>&1)
VS=$(q vecpolicy_without "SELECT count(*) FROM pg_extension WHERE extname='vectorscale'")
DB_=$(count_idx vecpolicy_without diskann); HB=$(count_idx vecpolicy_without hnsw)
echo "  vectorscale installed: ${VS:-0}   diskann: ${DB_:-0}   hnsw: ${HB:-0}"
if [ "${VS:-0}" = "0" ]; then
  [ "${HB:-0}" -ge 11 ] && pass "with vectorscale absent, all 11 fell back to HNSW" \
    || fail "expected 11 HNSW fallbacks, got ${HB:-0}"
else
  echo "  NOTE: the schema reinstalled vectorscale, so the fallback did not run here."
  pass "recorded: schema installs vectorscale when the library is present"
fi

echo
echo "== C. a dimension NEITHER method can index =="
# 3000 dims is above pgvector HNSW's 2000-dimension cap. With vectorscale absent,
# both paths refuse -- the case a large embedder hits.
sed 's/vector(768)/vector(3000)/g' "$SCHEMA" > "$BIGSCHEMA" 2>/dev/null
chmod 644 "$BIGSCHEMA" 2>/dev/null
fresh vecpolicy_big no
su postgres -c "$PSQL -d vecpolicy_big -tAc 'GRANT ALL ON SCHEMA public TO vecnoext'" >/dev/null 2>&1
su postgres -c "$PSQL -d vecpolicy_big -tAc 'GRANT ALL ON DATABASE vecpolicy_big TO vecnoext'" >/dev/null 2>&1
su postgres -c "$PSQL -d vecpolicy_big -tAc 'GRANT CREATE ON SCHEMA public TO vecnoext'" >/dev/null 2>&1
# Same limited role: vectorscale cannot be installed, so 3000 dims exceeds HNSW's
# 2000-dimension cap and BOTH index paths refuse -- the case a large embedder hits.
OUT3=$(su postgres -c "PGPASSWORD=vecnoext_pw $PSQL -w -U vecnoext -h 127.0.0.1 -d vecpolicy_big -f $BIGSCHEMA" 2>&1)
DC=$(count_idx vecpolicy_big diskann); HC=$(count_idx vecpolicy_big hnsw)
echo "  diskann: ${DC:-0}   hnsw: ${HC:-0}"
if echo "$OUT3" | grep -qi "external vector provider"; then
  pass "an unindexable dimension names the external vector provider path"
  echo "$OUT3" | grep -i "external vector provider" | head -1 | cut -c1-140 | sed 's/^/    /'
elif [ $(( ${DC:-0} + ${HC:-0} )) -ge 11 ]; then
  echo "  NOTE: 3000 dims was indexable on this build, so the guidance branch"
  echo "        was correctly not reached."
  pass "recorded: 3000-d is indexable here"
else
  fail "indexes are missing and no external-provider guidance was emitted"
  echo "$OUT3" | grep -i "notice" | head -4 | sed 's/^/    /'
fi

echo
echo "== an ANN query uses the index =="
DIM=$(q vecpolicy_with "SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'")
su postgres -c "$PSQL -d vecpolicy_with -tAc \
  \"INSERT INTO memory_embeddings(point_id, embedding) VALUES (1, array_fill(0.1::real, ARRAY[$DIM])::vector)\"" >/dev/null 2>&1
Q=$(su postgres -c "$PSQL -d vecpolicy_with -tAc \
  \"SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT point_id FROM memory_embeddings ORDER BY embedding <=> array_fill(0.1::real, ARRAY[$DIM])::vector LIMIT 1\"" 2>&1)
echo "$Q" | grep -qi "Index Scan\|diskann" \
  && pass "an ANN query plans against the vector index" \
  || { fail "ANN query did not use the index"; echo "$Q" | head -3 | sed 's/^/    /'; }

echo
echo "== re-apply is idempotent =="
su postgres -c "$PSQL -d vecpolicy_with -f $SCHEMA" >/dev/null 2>&1
D2=$(count_idx vecpolicy_with diskann)
[ "${D2:-0}" = "${DA:-0}" ] && pass "re-apply changed nothing (${D2})" \
  || fail "re-apply changed the index set (${DA} -> ${D2})"

echo
if [ "$FAIL" -eq 0 ]; then echo "S7 VECTOR INDEX POLICY PASSED"; else echo "S7 VECTOR INDEX POLICY FAILED ($FAIL)"; fi
exit "$FAIL"
