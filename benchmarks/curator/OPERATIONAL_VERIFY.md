# Deep-curator operational verification

End-to-end verification of the deep-curator passes against a **live**
`aimee-kb` + Postgres/pgvector DB2. The curator passes run only inside the
`aimee-kb` drain (there is no CLI to trigger a single pass), so this exercises
the real service: configure → seed → run the drain → assert.

The two LLM passes use the **deterministic stub sidecars** in
`benchmarks/curator/sidecars/`, so the whole suite runs **without a GPU/LLM**.
Point `judge_command` / `synthesize_command` at a real model to validate
against one instead.

This is a runbook, not a CI gate; it needs a real pgvector DB2, which CI does
not provide. Run it on a scratch DB2 (e.g. the CT 101 scratch-Postgres recipe);
it never touches a production aimee server.

## Prerequisites

- A scratch Postgres with the `vector` extension, reachable by `aimee-kb`.
- An `aimee-kb` binary built from this tree.
- `python3` (for the stub sidecars) and `psql`.

## 1. Configure (`aimee.yaml`)

Use **block style**; aimee's YAML parser is indentation-based and does not
parse flow maps (`{ enabled: true }`), so a flow-style gate reads as *off*:

```yaml
embedding_command: builtin          # MiniLM builtin embedder, no GPU needed
kb:
  curator:
    resolve_entities:
      enabled: true
    promote_entity:
      enabled: true
      min_sources: 3
    synthesize:
      enabled: true
      k: 8
    judge_command: "python3 BENCH/sidecars/judge_stub.py"
    synthesize_command: "python3 BENCH/sidecars/synthesize_stub.py"
    max_jobs_per_hour: 1000          # don't rate-limit the test
```

Replace `BENCH` with the absolute path to `benchmarks/curator`. DB2 connection
comes from the usual aimee DB2 config; point it at the scratch database.

## 2. Bootstrap the schema

```sh
aimee-kb --bootstrap-db2 --json     # creates artifacts, artifact_links,
                                    # audit_events, curator_*_vectors, projects
```

## 3. Seed a corpus (`psql`)

```sql
-- projects row so the promote lattice can resolve project -> workspace
INSERT INTO projects (name, root, workspace, scanned_at)
VALUES ('proj-a', '/tmp/proj-a', 'ws-1', '');

-- AC1: two entity mentions of the same real-world thing at different scopes.
-- resolve_entities embeds both; the second must dedup (>=0.85 merge, or the
-- 0.70-0.85 judge band via judge_stub) onto the first, NOT a second vector.
INSERT INTO artifacts (id, kind, state, scope_kind, scope_id, payload_json)
VALUES
 ('ent-acme-ws',  'entity', 'proposed', 'workspace', 'ws-1',
   '{"name":"Acme Corp","context":"the vendor"}'),
 ('ent-acme-proj','entity', 'proposed', 'project',   'proj-a',
   '{"name":"Acme","context":"vendor we buy from"}');

-- AC2/AC3: a project entity cited by 3 distinct sources -> promote + synthesize.
INSERT INTO artifacts (id, kind, state, scope_kind, scope_id, payload_json)
VALUES ('ent-widget', 'entity', 'committed', 'project', 'proj-a',
        '{"name":"Widget Service"}');
INSERT INTO artifacts (id, kind, state, scope_kind, scope_id, payload_json)
VALUES ('src-1','doc_summary','committed','project','proj-a','{"text":"uses Widget"}'),
       ('src-2','doc_summary','committed','project','proj-a','{"text":"Widget owns X"}'),
       ('src-3','claim','committed','project','proj-a','{"text":"Widget is core"}');
INSERT INTO artifact_links (from_id, to_id, kind) VALUES
 ('src-1','ent-widget','mentions'),
 ('src-2','ent-widget','mentions'),
 ('src-3','ent-widget','mentions');
```

## 4. Run the drain

```sh
aimee-kb --http-port=8899 --log-level=info &      # http only needed for AC4
sleep 30                                          # several 5s drain polls
```

## 5. Assertions

```sql
-- AC1 resolve-to-existing: the Acme cluster yields ONE canonical vector, not two.
SELECT count(*) AS acme_vectors FROM curator_entity_vectors
WHERE canonical_name ILIKE 'acme%';
-- EXPECT: 1   (the project mention deduped onto the workspace entity)

-- AC2 promote_entity: old entity superseded -> new entity at a broader scope,
-- with a supersedes link and an audit row.
SELECT (SELECT count(*) FROM artifact_links
          WHERE from_id='ent-widget' AND kind='supersedes')          AS supersedes,
       (SELECT count(*) FROM audit_events
          WHERE source_artifact_id='ent-widget'
            AND target_surface='kb.curator.promote_entity')          AS audit_rows,
       (SELECT scope_kind FROM artifacts a
          JOIN artifact_links l ON l.to_id=a.id AND l.kind='supersedes'
          WHERE l.from_id='ent-widget')                              AS promoted_scope;
-- EXPECT: supersedes=1, audit_rows=1, promoted_scope='workspace' (ws-1)

-- AC3 synthesize_topic: a synthesis artifact exists, linked `about` a hub entity.
SELECT count(*) AS syntheses FROM artifacts WHERE kind='synthesis' AND state='committed';
SELECT count(*) AS about_links FROM artifact_links WHERE kind='about';
-- EXPECT: syntheses>=1, about_links>=1
```

```sh
# AC4 (only if the /v1/synthesize ergonomic endpoint is present on this build):
curl -s -XPOST localhost:8899/v1/synthesize -d '{"topic":"Widget Service"}' | head
```

## 6. Teardown

```sh
kill %1                       # stop aimee-kb
dropdb <scratch-db>           # discard the scratch DB2
```

## Notes

- `judge_stub.py` decides "same entity" when names share a token (Acme / Acme
  Corp); pin with `JUDGE_STUB_FORCE=true|false`.
- `synthesize_stub.py` emits a deterministic synthesis referencing the topic +
  source count, so AC3 is assertable without a model.
- promote runs before synthesize can pick the promoted entity; both converge
  because each writes a marker (`supersedes` / `about`) that removes its
  candidate from the next poll.
