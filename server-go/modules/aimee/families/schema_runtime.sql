-- Runtime family: the small stores a running daemon keeps -- key/value state,
-- caches, local identity and capability records, the model catalog, file
-- snapshots, and the OSV advisory cache.
--
-- Most of these are caches, and their timestamps are FRESHNESS rather than
-- creation: rewriting an entry is supposed to move them. That is the opposite
-- of the tables elsewhere in this store where created_at means when a thing
-- came into being and must survive an update, and the difference is called out
-- wherever it could be mistaken.

-- --------------------------------------------------------------------------
-- Key/value runtime state
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS memory_runtime_state (
  state_key   TEXT PRIMARY KEY,
  state_value TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS env_capabilities (
  key         TEXT PRIMARY KEY,
  value       TEXT NOT NULL DEFAULT '',
  -- When the capability was last DETECTED. Re-detecting moves it, which is the
  -- point: a stale detection is what this column exists to reveal.
  detected_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS maintenance_state (
  key               TEXT PRIMARY KEY,
  last_run_at       TIMESTAMPTZ,
  last_memory_count BIGINT NOT NULL DEFAULT 0 CHECK (last_memory_count >= 0),
  last_changes      BIGINT NOT NULL DEFAULT 0 CHECK (last_changes >= 0),
  last_elapsed_ms   DOUBLE PRECISION NOT NULL DEFAULT 0 CHECK (last_elapsed_ms >= 0),
  last_summary_json TEXT NOT NULL DEFAULT ''
);

-- --------------------------------------------------------------------------
-- Local identity and clones
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS local_operator (
  secret_ref    TEXT PRIMARY KEY,
  operator_uuid TEXT NOT NULL,
  active        BOOLEAN NOT NULL DEFAULT false,
  display_hint  TEXT NOT NULL DEFAULT '',
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),

  -- At most one operator is active.
  --
  -- The C enforced this by clearing every other row and then setting one, from
  -- two statements -- so in between nobody was active, and if the second failed
  -- nobody was active for good.
  --
  -- Saying it as a constraint needs care, because switching operators must pass
  -- through a moment where two rows look active. A partial unique INDEX cannot
  -- be deferred, so it would reject the switch itself. A unique CONSTRAINT can
  -- be, and it works on a nullable marker: nulls do not conflict with each
  -- other, so only the active row participates, and DEFERRABLE moves the check
  -- to commit -- by which time the switch has finished.
  active_marker BOOLEAN GENERATED ALWAYS AS (CASE WHEN active THEN true END) STORED,
  CONSTRAINT local_operator_one_active UNIQUE (active_marker) DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE IF NOT EXISTS project_clones (
  clone_path    TEXT PRIMARY KEY,
  project_uuid  TEXT NOT NULL,
  canonical_url TEXT NOT NULL DEFAULT '',
  origin_url    TEXT NOT NULL DEFAULT '',
  upstream_url  TEXT NOT NULL DEFAULT '',
  last_seen_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS project_clones_project
  ON project_clones (project_uuid, last_seen_at DESC);

-- --------------------------------------------------------------------------
-- The model catalog and prices
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS model_catalog (
  provider          TEXT NOT NULL,
  model             TEXT NOT NULL,
  context_window    BIGINT NOT NULL DEFAULT 0 CHECK (context_window >= 0),
  pricing_tier      BIGINT NOT NULL DEFAULT 0,
  tool_support      BOOLEAN NOT NULL DEFAULT false,
  streaming_support BOOLEAN NOT NULL DEFAULT false,
  max_output        BIGINT NOT NULL DEFAULT 0 CHECK (max_output >= 0),
  caps              BIGINT NOT NULL DEFAULT 0,
  display_name      TEXT NOT NULL DEFAULT '',
  deprecated        BOOLEAN NOT NULL DEFAULT false,
  -- Freshness: is_fresh asks how old this is.
  fetched_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
  metadata_json     TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY (provider, model)
);

CREATE INDEX IF NOT EXISTS model_catalog_provider ON model_catalog (provider, model);

CREATE TABLE IF NOT EXISTS model_pricing (
  model             TEXT PRIMARY KEY,
  -- Money. NUMERIC rather than a binary float, for the same reason the token
  -- ledger is: these rates multiply out into what a bill says.
  cost_in_per_mtok  NUMERIC(20, 10) NOT NULL DEFAULT 0 CHECK (cost_in_per_mtok >= 0),
  cost_out_per_mtok NUMERIC(20, 10) NOT NULL DEFAULT 0 CHECK (cost_out_per_mtok >= 0),
  updated_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- --------------------------------------------------------------------------
-- Working profile observations
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS working_profile_observations_local (
  id                  BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  working_profile_key TEXT NOT NULL,
  session_id          TEXT NOT NULL DEFAULT '',
  signal              TEXT NOT NULL,
  payload_json        TEXT NOT NULL DEFAULT '{}',
  created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS working_profile_observations_key
  ON working_profile_observations_local (working_profile_key, id DESC);

CREATE TABLE IF NOT EXISTS working_profile_state_local (
  working_profile_key TEXT PRIMARY KEY,
  score               DOUBLE PRECISION NOT NULL DEFAULT 0,
  observation_count   BIGINT NOT NULL DEFAULT 0 CHECK (observation_count >= 0),
  last_observation_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tool_local_availability (
  tool_uuid   TEXT PRIMARY KEY,
  usable      BOOLEAN NOT NULL DEFAULT false,
  binary_path TEXT NOT NULL DEFAULT '',
  -- Freshness again: when the tool was last looked for.
  checked_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- --------------------------------------------------------------------------
-- Caches
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS context_cache (
  hash       TEXT PRIMARY KEY,
  output     TEXT NOT NULL,
  session_id TEXT NOT NULL DEFAULT '',
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS agent_cache (
  -- The role and prompt together are the key: the same prompt in the same role
  -- has the same answer. The C had a surrogate id and no uniqueness at all, so
  -- INSERT OR REPLACE never actually replaced anything and the table grew a new
  -- row per call while reads took whichever one they found first.
  role       TEXT NOT NULL,
  prompt     TEXT NOT NULL,
  result     TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (role, prompt)
);

CREATE TABLE IF NOT EXISTS web_page_cache (
  url          TEXT PRIMARY KEY,
  body         TEXT NOT NULL,
  byte_len     BIGINT NOT NULL DEFAULT 0 CHECK (byte_len >= 0),
  pinned_addr  TEXT NOT NULL DEFAULT '',
  fetched_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  -- The eviction order. Reading a page moves this; refetching moves both.
  last_used_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS web_page_cache_lru ON web_page_cache (last_used_at);

CREATE TABLE IF NOT EXISTS mcp_osv_cache (
  ecosystem    TEXT NOT NULL,
  name         TEXT NOT NULL,
  version      TEXT NOT NULL DEFAULT '',
  verdict      TEXT NOT NULL,
  advisory_ids TEXT NOT NULL DEFAULT '',
  -- Epoch seconds: the caller compares this against its own clock when
  -- deciding whether to re-query the advisory service.
  checked_at   BIGINT NOT NULL,
  PRIMARY KEY (ecosystem, name, version)
);

-- --------------------------------------------------------------------------
-- Context snapshots and decisions
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS context_snapshots (
  id              BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  session_id      TEXT NOT NULL,
  memory_id       BIGINT NOT NULL,
  relevance_score DOUBLE PRECISION NOT NULL DEFAULT 0,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS context_snapshots_memory ON context_snapshots (memory_id, id);
CREATE INDEX IF NOT EXISTS context_snapshots_session ON context_snapshots (session_id, id);

-- Retrieval activation has its own event stream. It must not share
-- context_snapshots: those rows are the effectiveness sample population, and
-- recording an injection there would turn exposure into reinforcement.
--
-- Retrieval hysteresis needs "how many turns ago", not "was this ever
-- injected": sticky keeps a unit eligible for N turns after it fires, and
-- cooldown refuses to fire it again for M. Neither question can be answered
-- from a row's existence, and answering it from wall-clock time would make the
-- gate depend on how fast someone types.
--
-- It lives here rather than in the assembling process because activation held
-- in process memory silently resets its cooldowns on restart, and the
-- repetition then returns with no visible cause. Turn numbering only has to be
-- monotonic within a session. The atomic UPSERT below allocates one index per
-- assembly, so concurrent requests cannot silently share a turn or lose an
-- increment.
--
CREATE TABLE IF NOT EXISTS context_activation_turns (
  session_id   TEXT PRIMARY KEY,
  current_turn BIGINT NOT NULL DEFAULT 0 CHECK (current_turn >= 0)
);

CREATE TABLE IF NOT EXISTS context_activation_events (
  id              BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  session_id      TEXT NOT NULL,
  memory_id       BIGINT NOT NULL,
  relevance_score DOUBLE PRECISION NOT NULL DEFAULT 0,
  turn_index      BIGINT NOT NULL CHECK (turn_index > 0),
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS context_activation_events_state
  ON context_activation_events (session_id, memory_id, turn_index DESC);

CREATE TABLE IF NOT EXISTS decisions (
  id          BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  window_id   BIGINT NOT NULL,
  description TEXT NOT NULL,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS decisions_window ON decisions (window_id, id);

-- --------------------------------------------------------------------------
-- File snapshots
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS file_snapshots (
  id         BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  session_id TEXT   NOT NULL DEFAULT '',
  turn       BIGINT NOT NULL DEFAULT 0 CHECK (turn >= 0),
  label      TEXT   NOT NULL DEFAULT '',
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  -- get_or_create looks a snapshot up by exactly this. Without the constraint
  -- two concurrent calls both miss the read and both insert, and the session
  -- ends up with two snapshots of the same turn -- one of which the restore
  -- will not see.
  UNIQUE (session_id, turn, label)
);

CREATE TABLE IF NOT EXISTS file_snapshot_entries (
  id          BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  snapshot_id BIGINT NOT NULL REFERENCES file_snapshots (id) ON DELETE CASCADE,
  path        TEXT   NOT NULL,
  -- Whether the file existed when the snapshot was taken. A file that did NOT
  -- exist is a real thing to record: restoring the snapshot has to delete it
  -- again, and a missing row would leave it behind.
  existed     BOOLEAN NOT NULL DEFAULT true,
  content     BYTEA,
  -- One row per path per snapshot: recording the same path twice would restore
  -- whichever the query happened to reach first.
  UNIQUE (snapshot_id, path),
  -- A file that existed has content; one that did not has none.
  CONSTRAINT file_snapshot_entries_content_when_existed
    CHECK (existed = (content IS NOT NULL))
);

CREATE INDEX IF NOT EXISTS file_snapshot_entries_snapshot
  ON file_snapshot_entries (snapshot_id);
