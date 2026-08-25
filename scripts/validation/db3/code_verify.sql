-- Does the routed form of the code search answer the same question as the
-- pgvector form?
--
-- The pgvector query expresses "current" as a JOIN: projects.lifecycle_state =
-- 'current' AND ce.generation = p.current_generation. A DB3 provider cannot
-- join, so pgvec_code_search resolves the project's current generation once and
-- sends it as an exact filter. The claim is that the two are equivalent because
-- projects.name is unique. This checks the claim against real rows, including
-- the three cases where an equivalence argument usually breaks: a retired
-- generation, a detached project, and a vector whose project row is gone.
\set ON_ERROR_STOP on
BEGIN;

CREATE TEMP TABLE t_projects (
    name                TEXT PRIMARY KEY,
    lifecycle_state     TEXT NOT NULL,
    current_generation  BIGINT NOT NULL
) ON COMMIT DROP;

CREATE TEMP TABLE t_code_embeddings (
    point_id    BIGINT PRIMARY KEY,
    embedding   vector(3),
    project     TEXT NOT NULL,
    generation  BIGINT
) ON COMMIT DROP;

INSERT INTO t_projects VALUES
    ('alpha', 'current',  2),   -- has a retired generation 1 beneath it
    ('beta',  'detached', 5);   -- detached: the join returns nothing for it

INSERT INTO t_code_embeddings VALUES
    (1, '[1,0,0]',   'alpha', 2),      -- current, should be found
    (2, '[0.9,0.1,0]','alpha', 2),     -- current, should be found
    (3, '[1,0,0]',   'alpha', 1),      -- RETIRED generation, must not be found
    (4, '[1,0,0]',   'beta',  5),      -- detached project, must not be found
    (5, '[1,0,0]',   'gamma', 1),      -- ORPHAN: no projects row at all
    (6, '[1,0,0]',   'alpha', NULL);   -- no generation: orphaned vector

-- The join form, as pgvec_code_search_pgvector asks it.
CREATE TEMP VIEW join_form AS
SELECT ce.point_id
  FROM t_code_embeddings ce
  JOIN t_projects p ON p.name = ce.project
 WHERE ce.project = 'alpha'
   AND p.lifecycle_state = 'current'
   AND ce.generation = p.current_generation;

-- The filter form, as the wrapper sends it: one resolved generation, then exact
-- equality. The resolution is the same SELECT project_current_generation runs.
CREATE TEMP VIEW filter_form AS
SELECT ce.point_id
  FROM t_code_embeddings ce
 WHERE ce.project = 'alpha'
   AND ce.generation = (SELECT current_generation FROM t_projects
                         WHERE name = 'alpha' AND lifecycle_state = 'current');

DO $$
DECLARE
    only_join  BIGINT[];
    only_filt  BIGINT[];
    found      BIGINT[];
    detached   BIGINT;
BEGIN
    SELECT array_agg(point_id ORDER BY point_id) INTO found FROM join_form;
    IF found IS DISTINCT FROM ARRAY[1::BIGINT, 2::BIGINT] THEN
        RAISE EXCEPTION 'join form returned %, expected {1,2}', found;
    END IF;

    SELECT array_agg(point_id ORDER BY point_id) INTO only_join
      FROM (SELECT point_id FROM join_form EXCEPT SELECT point_id FROM filter_form) s;
    SELECT array_agg(point_id ORDER BY point_id) INTO only_filt
      FROM (SELECT point_id FROM filter_form EXCEPT SELECT point_id FROM join_form) s;
    IF only_join IS NOT NULL OR only_filt IS NOT NULL THEN
        RAISE EXCEPTION 'forms disagree: only-join=% only-filter=%', only_join, only_filt;
    END IF;
    RAISE NOTICE 'ok: filter form and join form agree, and exclude the retired generation, the detached project, the orphan, and the NULL generation';

    -- A detached project must resolve to nothing, so the wrapper does not route
    -- at all. If this returned a row, the search would route without the
    -- lifecycle condition and answer from the generation it was detached at.
    SELECT current_generation INTO detached FROM t_projects
     WHERE name = 'beta' AND lifecycle_state = 'current';
    IF detached IS NOT NULL THEN
        RAISE EXCEPTION 'detached project resolved to generation %, must resolve to nothing', detached;
    END IF;
    RAISE NOTICE 'ok: a detached project resolves to nothing, so the search stays on pgvector';

    -- A project with no row at all resolves to nothing for the same reason.
    SELECT current_generation INTO detached FROM t_projects
     WHERE name = 'gamma' AND lifecycle_state = 'current';
    IF detached IS NOT NULL THEN
        RAISE EXCEPTION 'unknown project resolved to generation %', detached;
    END IF;
    RAISE NOTICE 'ok: an unknown project resolves to nothing';
END
$$;

-- The vector operator has to actually work on these rows: an equivalence that
-- only holds for the id sets but not the ordering would still ship the wrong
-- answer.
DO $$
DECLARE
    ordered BIGINT[];
BEGIN
    SELECT array_agg(point_id) INTO ordered FROM (
        SELECT ce.point_id
          FROM t_code_embeddings ce
         WHERE ce.project = 'alpha'
           AND ce.generation = (SELECT current_generation FROM t_projects
                                 WHERE name = 'alpha' AND lifecycle_state = 'current')
         ORDER BY ce.embedding <=> '[1,0,0]'::vector
    ) s;
    IF ordered IS DISTINCT FROM ARRAY[1::BIGINT, 2::BIGINT] THEN
        RAISE EXCEPTION 'distance ordering returned %, expected {1,2}', ordered;
    END IF;
    RAISE NOTICE 'ok: pgvector orders the routed candidate set nearest-first';
END
$$;

COMMIT;
