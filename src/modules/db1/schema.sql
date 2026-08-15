CREATE TABLE IF NOT EXISTS working_memory ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL DEFAULT '', category TEXT NOT NULL DEFAULT 'general', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), expires_at TEXT DEFAULT NULL, UNIQUE(session_id, key));
CREATE TABLE IF NOT EXISTS file_snapshots ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL DEFAULT '', turn INTEGER NOT NULL DEFAULT 0, label TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS file_snapshot_entries ( id INTEGER PRIMARY KEY AUTOINCREMENT, snapshot_id INTEGER NOT NULL REFERENCES file_snapshots(id) ON DELETE CASCADE, path TEXT NOT NULL, existed INTEGER NOT NULL DEFAULT 1, content BLOB);
CREATE TABLE IF NOT EXISTS checkpoints ( id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER DEFAULT 0, session_id TEXT, label TEXT NOT NULL, snapshot TEXT NOT NULL DEFAULT '{}', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS clarify_sessions ( id INTEGER PRIMARY KEY AUTOINCREMENT, description TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'open', score REAL NOT NULL DEFAULT 0, spec TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS clarify_qa ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL REFERENCES clarify_sessions(id) ON DELETE CASCADE, dimension TEXT NOT NULL DEFAULT '', question TEXT NOT NULL, answer TEXT NOT NULL DEFAULT '', answered INTEGER NOT NULL DEFAULT 0, seq INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS diagnoses ( id INTEGER PRIMARY KEY AUTOINCREMENT, symptom TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'active', conclusion TEXT NOT NULL DEFAULT '', confidence REAL NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS diagnosis_items ( id INTEGER PRIMARY KEY AUTOINCREMENT, diagnosis_id INTEGER NOT NULL REFERENCES diagnoses(id) ON DELETE CASCADE, kind TEXT NOT NULL, parent_id INTEGER NOT NULL DEFAULT 0, content TEXT NOT NULL, source TEXT NOT NULL DEFAULT '', evidence_rank INTEGER NOT NULL DEFAULT 4, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS ensembles ( id INTEGER PRIMARY KEY AUTOINCREMENT, template_name TEXT NOT NULL, channel TEXT NOT NULL DEFAULT 'general', status TEXT NOT NULL DEFAULT 'active', current_phase INTEGER NOT NULL DEFAULT 0, current_turn INTEGER NOT NULL DEFAULT 0, expected_agent TEXT NOT NULL DEFAULT '', expected_role TEXT NOT NULL DEFAULT '', paused_reason TEXT NOT NULL DEFAULT '', template_json TEXT NOT NULL DEFAULT '{}', assignments_json TEXT NOT NULL DEFAULT '{}', context_json TEXT NOT NULL DEFAULT '[]', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_ensembles_status ON ensembles(status, updated_at DESC);
CREATE TABLE IF NOT EXISTS context_cache ( hash TEXT PRIMARY KEY, output TEXT NOT NULL, session_id TEXT, created_at TEXT NOT NULL DEFAULT (datetime('now')));
-- Fetched web pages, STRIPPED TO TEXT, keyed by canonical URL.
-- Keyed by URL alone and NOT by (url, query, budget): extraction is a
-- deterministic pure function of (text, query, budget) that is re-run on every
-- hit, so the cache supplies the document and never freezes a policy decision.
-- pinned_addr is the address the egress guard validated and connected to at
-- fetch time; a hit re-checks it against the current deny-list so tightening
-- the policy retroactively invalidates entries it would now refuse.
CREATE TABLE IF NOT EXISTS web_page_cache (
  url          TEXT PRIMARY KEY,
  body         TEXT NOT NULL,
  byte_len     INTEGER NOT NULL DEFAULT 0,
  pinned_addr  TEXT NOT NULL DEFAULT '',
  fetched_at   TEXT NOT NULL DEFAULT (datetime('now')),
  last_used_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_web_page_cache_lru ON web_page_cache (last_used_at);
CREATE TABLE IF NOT EXISTS agent_cache ( id INTEGER PRIMARY KEY, role TEXT NOT NULL, prompt TEXT NOT NULL, result TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_agent_cache_lookup ON agent_cache(role, prompt);
CREATE TABLE IF NOT EXISTS primary_sessions ( session_id TEXT NOT NULL, agent_name TEXT NOT NULL DEFAULT '', provider TEXT NOT NULL DEFAULT '', messages_json TEXT NOT NULL DEFAULT '[]', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), PRIMARY KEY (session_id, agent_name, provider));
CREATE INDEX IF NOT EXISTS idx_primary_sessions_updated ON primary_sessions(updated_at DESC);
CREATE TABLE IF NOT EXISTS webchat_claude_sessions ( principal TEXT NOT NULL DEFAULT '', aimee_session_id TEXT NOT NULL, claude_session_id TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT (datetime('now')), PRIMARY KEY (principal, aimee_session_id));
CREATE INDEX IF NOT EXISTS idx_webchat_claude_sessions_csid ON webchat_claude_sessions(claude_session_id);
CREATE TABLE IF NOT EXISTS webchat_live ( session_id TEXT PRIMARY KEY, turn_id TEXT NOT NULL DEFAULT '', rev INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'idle', updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS env_capabilities ( key TEXT PRIMARY KEY, value TEXT NOT NULL DEFAULT '', detected_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS maintenance_state ( key TEXT PRIMARY KEY, last_run_at TEXT NOT NULL DEFAULT '', last_memory_count INTEGER NOT NULL DEFAULT 0, last_changes INTEGER NOT NULL DEFAULT 0, last_elapsed_ms REAL NOT NULL DEFAULT 0, last_summary_json TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS wc_channels ( id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS wc_channel_messages ( id INTEGER PRIMARY KEY AUTOINCREMENT, channel_name TEXT NOT NULL, sender TEXT NOT NULL, text TEXT NOT NULL, mentions TEXT NOT NULL DEFAULT '[]', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_wcm_channel ON wc_channel_messages(channel_name, id);
CREATE TABLE IF NOT EXISTS delegation_messages ( id INTEGER PRIMARY KEY AUTOINCREMENT, delegation_id TEXT NOT NULL DEFAULT '', direction TEXT NOT NULL DEFAULT '', content TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS delegation_checkpoint ( delegation_id TEXT PRIMARY KEY, job_id TEXT NOT NULL DEFAULT '', steps_completed TEXT NOT NULL DEFAULT '[]', last_output TEXT NOT NULL DEFAULT '', error TEXT NOT NULL DEFAULT '', attempt INTEGER NOT NULL DEFAULT 0, failed_at INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS delegation_spawns ( id INTEGER PRIMARY KEY AUTOINCREMENT, delegation_id TEXT NOT NULL DEFAULT '', parent_delegation_id TEXT NOT NULL DEFAULT '', session_id TEXT NOT NULL DEFAULT '', depth INTEGER NOT NULL DEFAULT 0, role TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'active', created_at TEXT NOT NULL DEFAULT (datetime('now')), completed_at TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS agent_log ( id INTEGER PRIMARY KEY AUTOINCREMENT, agent_name TEXT NOT NULL DEFAULT '', role TEXT NOT NULL, input TEXT NOT NULL DEFAULT '', output TEXT NOT NULL DEFAULT '', prompt_tokens INTEGER NOT NULL DEFAULT 0, completion_tokens INTEGER NOT NULL DEFAULT 0, latency_ms INTEGER NOT NULL DEFAULT 0, success INTEGER NOT NULL DEFAULT 0, error TEXT DEFAULT NULL, confidence INTEGER NOT NULL DEFAULT -1, turns INTEGER NOT NULL DEFAULT 0, tool_calls INTEGER NOT NULL DEFAULT 0, session_id TEXT DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS server_sessions ( id TEXT PRIMARY KEY, client_type TEXT NOT NULL DEFAULT '', principal TEXT NOT NULL DEFAULT '', title TEXT DEFAULT '', last_activity_at TEXT NOT NULL DEFAULT (datetime('now')), claude_session_id TEXT DEFAULT '', metadata TEXT DEFAULT '{}', outcome TEXT DEFAULT NULL, rule_violations INTEGER NOT NULL DEFAULT 0, source TEXT NOT NULL DEFAULT '', chat_key TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS session_state ( session_id TEXT PRIMARY KEY, session_mode TEXT NOT NULL DEFAULT 'implement', guardrail_mode TEXT NOT NULL DEFAULT 'approve', tdd_mode TEXT NOT NULL DEFAULT 'off', active_task_id INTEGER NOT NULL DEFAULT 0, hook_call_count INTEGER NOT NULL DEFAULT 0, orch_direct_edits INTEGER NOT NULL DEFAULT 0, orch_nudge_sent INTEGER NOT NULL DEFAULT 0, skill_find_symbols_advisory_sent INTEGER NOT NULL DEFAULT 0, skill_condition_waiting_advisory_sent INTEGER NOT NULL DEFAULT 0, skill_tdd_advisory_sent INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS session_state_seen_paths ( session_id TEXT NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE, seq INTEGER NOT NULL, path TEXT NOT NULL, PRIMARY KEY (session_id, seq));
CREATE TABLE IF NOT EXISTS session_state_read_paths ( session_id TEXT NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE, seq INTEGER NOT NULL, path TEXT NOT NULL, PRIMARY KEY (session_id, seq));
CREATE TABLE IF NOT EXISTS session_state_write_paths ( session_id TEXT NOT NULL, seq INTEGER NOT NULL, path TEXT NOT NULL, PRIMARY KEY (session_id, seq));
CREATE TABLE IF NOT EXISTS session_state_worktrees ( session_id TEXT NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE, seq INTEGER NOT NULL, git_root TEXT NOT NULL, worktree_path TEXT NOT NULL, PRIMARY KEY (session_id, seq));
CREATE TABLE IF NOT EXISTS session_state_tdd_writes ( session_id TEXT NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE, seq INTEGER NOT NULL, stem TEXT NOT NULL, is_test INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (session_id, seq));
CREATE TABLE IF NOT EXISTS session_state_ap_hits ( session_id TEXT NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE, pattern_id INTEGER NOT NULL, hits INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (session_id, pattern_id));
CREATE TABLE IF NOT EXISTS session_state_file_hashes ( session_id TEXT NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE, path TEXT NOT NULL, content_hash TEXT NOT NULL, PRIMARY KEY (session_id, path));
CREATE TABLE IF NOT EXISTS decisions ( id INTEGER PRIMARY KEY AUTOINCREMENT, window_id INTEGER NOT NULL, description TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS eval_results ( id INTEGER PRIMARY KEY AUTOINCREMENT, suite TEXT NOT NULL, task_name TEXT NOT NULL DEFAULT '', agent_name TEXT NOT NULL DEFAULT '', ablation TEXT NOT NULL DEFAULT 'full', success INTEGER NOT NULL DEFAULT 0, turns INTEGER NOT NULL DEFAULT 0, tool_calls INTEGER NOT NULL DEFAULT 0, tool_call_failures INTEGER NOT NULL DEFAULT 0, rescue_recoveries INTEGER NOT NULL DEFAULT 0, prompt_tokens INTEGER NOT NULL DEFAULT 0, completion_tokens INTEGER NOT NULL DEFAULT 0, latency_ms INTEGER NOT NULL DEFAULT 0, response TEXT NOT NULL DEFAULT '', error TEXT, dataset_hash TEXT NOT NULL DEFAULT '', target_hash TEXT NOT NULL DEFAULT '', harness_version TEXT NOT NULL DEFAULT '1', hardware_profile TEXT NOT NULL DEFAULT '', seed INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS memory_runtime_state ( state_key TEXT PRIMARY KEY, state_value TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS server_mgmt_nonce (
  nonce BLOB PRIMARY KEY CHECK(length(nonce)=32),
  peer_issuer TEXT NOT NULL,
  peer_serial_norm TEXT NOT NULL,
  peer_fingerprint TEXT NOT NULL,
  channel_binding TEXT NOT NULL,
  target_server_id TEXT NOT NULL,
  purpose TEXT NOT NULL,
  expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_server_mgmt_nonce_expiry ON server_mgmt_nonce(expires_at);
CREATE TABLE IF NOT EXISTS server_mgmt_status_hwm (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  generation INTEGER NOT NULL CHECK(generation>=0)
);
INSERT OR IGNORE INTO server_mgmt_status_hwm(singleton,generation) VALUES(1,0);
CREATE TABLE IF NOT EXISTS server_management_jti (
  jti TEXT PRIMARY KEY NOT NULL CHECK(
    typeof(jti)='text' AND length(jti) BETWEEN 16 AND 128 AND instr(jti,char(0))=0 AND
    jti NOT GLOB '*[^A-Za-z0-9._-]*'),
  issuer TEXT NOT NULL CHECK(
    typeof(issuer)='text' AND length(issuer) BETWEEN 1 AND 255 AND instr(issuer,char(0))=0 AND
    issuer NOT GLOB ('*[' || char(1) || '-' || char(31) || char(127) || ']*')),
  kid TEXT NOT NULL CHECK(
    typeof(kid)='text' AND length(kid) BETWEEN 1 AND 64 AND instr(kid,char(0))=0 AND
    kid NOT GLOB '*[^A-Za-z0-9._-]*'),
  audience TEXT NOT NULL CHECK(
    typeof(audience)='text' AND length(audience) BETWEEN 1 AND 127 AND instr(audience,char(0))=0 AND
    audience NOT GLOB '*[^A-Za-z0-9._-]*'),
  subject TEXT NOT NULL CHECK(
    typeof(subject)='text' AND length(subject) BETWEEN 1 AND 576 AND instr(subject,char(0))=0 AND
    subject NOT GLOB ('*[' || char(1) || '-' || char(31) || char(127) || ']*')),
  team_id INTEGER NOT NULL CHECK(typeof(team_id)='integer' AND team_id > 0),
  capability TEXT NOT NULL CHECK(
    typeof(capability)='text' AND length(capability) BETWEEN 1 AND 64 AND instr(capability,char(0))=0 AND
    capability NOT GLOB '*[^A-Za-z0-9._-]*'),
  peer_issuer TEXT NOT NULL CHECK(
    typeof(peer_issuer)='text' AND length(peer_issuer) BETWEEN 1 AND 511 AND instr(peer_issuer,char(0))=0 AND
    peer_issuer NOT GLOB ('*[' || char(1) || '-' || char(31) || char(127) || ']*')),
  peer_serial TEXT NOT NULL CHECK(
    typeof(peer_serial)='text' AND length(peer_serial) BETWEEN 1 AND 79 AND instr(peer_serial,char(0))=0 AND
    peer_serial NOT GLOB '*[^0-9a-f]*'),
  peer_fingerprint TEXT NOT NULL CHECK(
    typeof(peer_fingerprint)='text' AND length(peer_fingerprint)=64 AND instr(peer_fingerprint,char(0))=0 AND
    peer_fingerprint NOT GLOB '*[^0-9a-f]*'),
  request_sha256 TEXT NOT NULL CHECK(
    typeof(request_sha256)='text' AND length(request_sha256)=64 AND instr(request_sha256,char(0))=0 AND
    request_sha256 NOT GLOB '*[^0-9a-f]*'),
  correlation_id TEXT NOT NULL CHECK(
    typeof(correlation_id)='text' AND length(correlation_id) BETWEEN 1 AND 128 AND instr(correlation_id,char(0))=0 AND
    correlation_id NOT GLOB '*[^A-Za-z0-9._-]*'),
  issued_at INTEGER NOT NULL CHECK(typeof(issued_at)='integer' AND issued_at >= 0),
  expires_at INTEGER NOT NULL CHECK(typeof(expires_at)='integer' AND expires_at > issued_at),
  consumed_at INTEGER NOT NULL CHECK(typeof(consumed_at)='integer' AND consumed_at >= issued_at AND consumed_at < expires_at)
);
CREATE INDEX IF NOT EXISTS idx_server_management_jti_expiry
  ON server_management_jti(expires_at,jti);

-- Replay store for the data-plane identity token (proposal
-- per-user-remote-writes-authz.md §4/§9). A SIBLING of server_management_jti
-- rather than a reuse of it: that table's peer_issuer / peer_serial /
-- peer_fingerprint / request_sha256 columns are NOT NULL because a management
-- token genuinely carries all four. An identity token carries none of them - it
-- has no peer certificate and no request digest - so reusing the table would
-- mean writing placeholder provenance into a security-audit table, or relaxing
-- constraints that are the reason that table is trustworthy. Neither is
-- acceptable, so the identity token gets its own shape: a tier instead of a
-- capability, and no peer or request binding at all.
--
-- The jti floor is 8 rather than 16, matching what the server's identity
-- verifier accepts (ascii_token 8..128).
CREATE TABLE IF NOT EXISTS server_identity_jti (
  jti TEXT PRIMARY KEY NOT NULL CHECK(
    typeof(jti)='text' AND length(jti) BETWEEN 8 AND 128 AND instr(jti,char(0))=0 AND
    jti NOT GLOB '*[^A-Za-z0-9._-]*'),
  issuer TEXT NOT NULL CHECK(
    typeof(issuer)='text' AND length(issuer) BETWEEN 1 AND 255 AND instr(issuer,char(0))=0 AND
    issuer NOT GLOB ('*[' || char(1) || '-' || char(31) || char(127) || ']*')),
  kid TEXT NOT NULL CHECK(
    typeof(kid)='text' AND length(kid) BETWEEN 1 AND 64 AND instr(kid,char(0))=0 AND
    kid NOT GLOB '*[^A-Za-z0-9._-]*'),
  audience TEXT NOT NULL CHECK(
    typeof(audience)='text' AND length(audience) BETWEEN 1 AND 127 AND instr(audience,char(0))=0 AND
    audience NOT GLOB '*[^A-Za-z0-9._-]*'),
  subject TEXT NOT NULL CHECK(
    typeof(subject)='text' AND length(subject) BETWEEN 1 AND 576 AND instr(subject,char(0))=0 AND
    subject NOT GLOB ('*[' || char(1) || '-' || char(31) || char(127) || ']*')),
  team_id INTEGER NOT NULL CHECK(typeof(team_id)='integer' AND team_id > 0),
  tier TEXT NOT NULL CHECK(typeof(tier)='text' AND tier IN ('off','data','full')),
  issued_at INTEGER NOT NULL CHECK(typeof(issued_at)='integer' AND issued_at >= 0),
  expires_at INTEGER NOT NULL CHECK(typeof(expires_at)='integer' AND expires_at > issued_at),
  consumed_at INTEGER NOT NULL CHECK(typeof(consumed_at)='integer' AND consumed_at >= issued_at AND consumed_at < expires_at)
);
CREATE INDEX IF NOT EXISTS idx_server_identity_jti_expiry
  ON server_identity_jti(expires_at,jti);
CREATE TABLE IF NOT EXISTS server_management_jwks_cache (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  generation INTEGER NOT NULL CHECK(generation=1),
  valid_from INTEGER NOT NULL CHECK(valid_from>=0),
  valid_until INTEGER NOT NULL CHECK(valid_until>valid_from),
  jwks_bytes BLOB NOT NULL CHECK(length(jwks_bytes) BETWEEN 1 AND 1023),
  envelope_bytes BLOB NOT NULL CHECK(length(envelope_bytes) BETWEEN 1 AND 3071),
  envelope_sha256 BLOB NOT NULL CHECK(length(envelope_sha256)=32),
  manifest_sha256 BLOB NOT NULL CHECK(length(manifest_sha256)=32),
  trust_bundle_sha256 BLOB NOT NULL CHECK(length(trust_bundle_sha256)=32),
  fetched_at INTEGER NOT NULL CHECK(fetched_at>=0)
);
CREATE TABLE IF NOT EXISTS token_audit ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL DEFAULT '', delegation_id TEXT NOT NULL DEFAULT '', project_name TEXT NOT NULL DEFAULT '', tool_name TEXT NOT NULL DEFAULT '', role TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '', source TEXT NOT NULL DEFAULT '', requested_model TEXT NOT NULL DEFAULT '', stop_reason TEXT NOT NULL DEFAULT '', usage_kind TEXT NOT NULL DEFAULT 'realized', agent_log_id INTEGER NOT NULL DEFAULT 0, request_id TEXT NOT NULL DEFAULT '', idempotency_key TEXT NOT NULL DEFAULT '', attempt INTEGER NOT NULL DEFAULT 0, principal TEXT NOT NULL DEFAULT '', served_model TEXT NOT NULL DEFAULT '', duration_ms INTEGER NOT NULL DEFAULT 0, metadata TEXT NOT NULL DEFAULT '', prompt_tokens INTEGER NOT NULL DEFAULT 0, completion_tokens INTEGER NOT NULL DEFAULT 0, cache_write_tokens INTEGER NOT NULL DEFAULT 0, cache_read_tokens INTEGER NOT NULL DEFAULT 0, estimated_cost_usd REAL NOT NULL DEFAULT 0.0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS model_catalog ( provider TEXT NOT NULL, model TEXT NOT NULL, context_window INTEGER NOT NULL DEFAULT 0, pricing_tier INTEGER NOT NULL DEFAULT 0, tool_support INTEGER NOT NULL DEFAULT 0, streaming_support INTEGER NOT NULL DEFAULT 0, max_output INTEGER NOT NULL DEFAULT 0, caps INTEGER NOT NULL DEFAULT 0, display_name TEXT NOT NULL DEFAULT '', deprecated INTEGER NOT NULL DEFAULT 0, fetched_at TEXT NOT NULL DEFAULT (datetime('now')), metadata_json TEXT NOT NULL DEFAULT '{}', PRIMARY KEY (provider, model));
CREATE TABLE IF NOT EXISTS model_pricing ( model TEXT PRIMARY KEY, cost_in_per_mtok REAL NOT NULL DEFAULT 0 CHECK (cost_in_per_mtok >= 0), cost_out_per_mtok REAL NOT NULL DEFAULT 0 CHECK (cost_out_per_mtok >= 0), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_model_catalog_fetched ON model_catalog(provider, fetched_at);
CREATE TABLE IF NOT EXISTS agent_jobs ( id INTEGER PRIMARY KEY AUTOINCREMENT, role TEXT NOT NULL DEFAULT '', prompt TEXT NOT NULL DEFAULT '', agent_name TEXT NOT NULL DEFAULT '', participant_token TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'pending', result TEXT NOT NULL DEFAULT '', cursor TEXT NOT NULL DEFAULT '', lease_owner TEXT NOT NULL DEFAULT '', heartbeat_at TEXT NOT NULL DEFAULT '', current_tool TEXT NOT NULL DEFAULT '', api_call_count INTEGER NOT NULL DEFAULT 0, cost_usd REAL NOT NULL DEFAULT 0, cost_known INTEGER NOT NULL DEFAULT 0, cancelled_at TEXT DEFAULT '', cancel_reason TEXT DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE UNIQUE INDEX IF NOT EXISTS idx_agent_jobs_participant_token ON agent_jobs(participant_token) WHERE participant_token <> '';
CREATE TABLE IF NOT EXISTS agent_log ( id INTEGER PRIMARY KEY AUTOINCREMENT, agent_name TEXT NOT NULL DEFAULT '', role TEXT NOT NULL, input TEXT NOT NULL DEFAULT '', output TEXT NOT NULL DEFAULT '', prompt_tokens INTEGER NOT NULL DEFAULT 0, completion_tokens INTEGER NOT NULL DEFAULT 0, latency_ms INTEGER NOT NULL DEFAULT 0, success INTEGER NOT NULL DEFAULT 0, error TEXT DEFAULT NULL, confidence INTEGER NOT NULL DEFAULT -1, turns INTEGER NOT NULL DEFAULT 0, tool_calls INTEGER NOT NULL DEFAULT 0, session_id TEXT DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS cost_fold_log ( id INTEGER PRIMARY KEY AUTOINCREMENT, parent_session_id TEXT NOT NULL, child_session_id TEXT NOT NULL, cost_usd REAL NOT NULL DEFAULT 0.0, cost_source TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(parent_session_id, child_session_id));
CREATE TABLE IF NOT EXISTS execution_plans ( id INTEGER PRIMARY KEY AUTOINCREMENT, agent_name TEXT NOT NULL DEFAULT '', task TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'pending', confidence REAL NOT NULL DEFAULT 1.0, cancelled_at TEXT DEFAULT '', cancel_reason TEXT DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS plan_steps ( id INTEGER PRIMARY KEY AUTOINCREMENT, plan_id INTEGER NOT NULL REFERENCES execution_plans(id) ON DELETE CASCADE, seq INTEGER NOT NULL, action TEXT NOT NULL, precondition TEXT DEFAULT '', success_predicate TEXT DEFAULT '', rollback TEXT DEFAULT '', status TEXT NOT NULL DEFAULT 'pending', output TEXT DEFAULT '', checkpoint TEXT DEFAULT '', deps TEXT DEFAULT '[]', started_at TEXT DEFAULT '', finished_at TEXT DEFAULT '');
CREATE TABLE IF NOT EXISTS step_evidence ( id INTEGER PRIMARY KEY AUTOINCREMENT, plan_id INTEGER NOT NULL, step_id INTEGER NOT NULL, kind TEXT NOT NULL, content TEXT NOT NULL, passed INTEGER NOT NULL DEFAULT 0, strength TEXT NOT NULL DEFAULT 'weak', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS coord_jobs ( id INTEGER PRIMARY KEY AUTOINCREMENT, plan_id INTEGER NOT NULL DEFAULT 0, status TEXT NOT NULL DEFAULT 'pending', max_concurrent INTEGER NOT NULL DEFAULT 3, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS coord_job_tasks ( id INTEGER PRIMARY KEY AUTOINCREMENT, job_id INTEGER NOT NULL REFERENCES coord_jobs(id) ON DELETE CASCADE, step_id INTEGER DEFAULT NULL, status TEXT NOT NULL DEFAULT 'pending', claimed_by TEXT NOT NULL DEFAULT '', claimed_at TEXT NOT NULL DEFAULT '', files TEXT NOT NULL DEFAULT '[]', role TEXT NOT NULL DEFAULT 'execute', persona TEXT NOT NULL DEFAULT 'engineer', prompt TEXT NOT NULL DEFAULT '', cwd TEXT NOT NULL DEFAULT '', result TEXT NOT NULL DEFAULT '', error TEXT NOT NULL DEFAULT '', preempt_requeues INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
-- Inter-session work queue: server coordination state, DB1-owned (moved from DB2).
CREATE TABLE IF NOT EXISTS pipelines ( id INTEGER PRIMARY KEY AUTOINCREMENT, task TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'active', current_phase TEXT NOT NULL DEFAULT 'plan', phase_attempts INTEGER NOT NULL DEFAULT 0, plan_id INTEGER NOT NULL DEFAULT 0, job_id INTEGER NOT NULL DEFAULT 0, request_classification TEXT NOT NULL DEFAULT 'simple', plan_depth TEXT NOT NULL DEFAULT 'simple', clarify_session_id INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS trigger_runs ( id TEXT PRIMARY KEY, source TEXT NOT NULL, event TEXT NOT NULL DEFAULT '', task TEXT NOT NULL, workspace TEXT NOT NULL DEFAULT '', metadata TEXT NOT NULL DEFAULT '{}', pipeline_id TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'queued', queued_at TEXT NOT NULL DEFAULT (datetime('now')), started_at TEXT DEFAULT '', finished_at TEXT DEFAULT '', error TEXT DEFAULT '');
CREATE INDEX IF NOT EXISTS idx_trigger_runs_status ON trigger_runs(status);
CREATE TABLE IF NOT EXISTS cron_jobs ( id TEXT PRIMARY KEY, schedule TEXT NOT NULL, mode TEXT NOT NULL CHECK (mode IN ('script','llm','hybrid')), script TEXT NOT NULL DEFAULT '', prompt TEXT NOT NULL DEFAULT '', skills_csv TEXT NOT NULL DEFAULT '', workdir TEXT NOT NULL DEFAULT '', deliver_target TEXT NOT NULL DEFAULT '', deliver_only_if_changed INTEGER NOT NULL DEFAULT 0, deliver_first_run_silent INTEGER NOT NULL DEFAULT 0, context_from TEXT NOT NULL DEFAULT '', when_context_contains TEXT NOT NULL DEFAULT '', pre_wake_gate INTEGER NOT NULL DEFAULT 0, enabled INTEGER NOT NULL DEFAULT 1, next_run_at INTEGER DEFAULT 0, last_run_at INTEGER DEFAULT 0, last_run_status TEXT NOT NULL DEFAULT '', last_run_output_hash TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')));
CREATE TABLE IF NOT EXISTS cron_job_runs ( id INTEGER PRIMARY KEY AUTOINCREMENT, job_id TEXT NOT NULL, started_at INTEGER NOT NULL DEFAULT (strftime('%s','now')), completed_at INTEGER DEFAULT 0, status TEXT NOT NULL DEFAULT '', silent INTEGER NOT NULL DEFAULT 0, delivered INTEGER NOT NULL DEFAULT 0, output TEXT NOT NULL DEFAULT '', error TEXT NOT NULL DEFAULT '', output_hash TEXT NOT NULL DEFAULT '', FOREIGN KEY (job_id) REFERENCES cron_jobs(id));
CREATE INDEX IF NOT EXISTS idx_cron_job_runs_job ON cron_job_runs(job_id, id DESC);
CREATE TABLE IF NOT EXISTS execution_trace ( id INTEGER PRIMARY KEY AUTOINCREMENT, plan_id INTEGER NOT NULL DEFAULT 0, session_id TEXT NOT NULL DEFAULT '', turn INTEGER NOT NULL DEFAULT 0, direction TEXT NOT NULL DEFAULT 'call', content TEXT NOT NULL DEFAULT '', tool_name TEXT NOT NULL DEFAULT '', tool_args TEXT NOT NULL DEFAULT '', tool_result TEXT NOT NULL DEFAULT '', context_hash TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS windows ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, seq INTEGER NOT NULL, summary TEXT NOT NULL DEFAULT '', tier TEXT NOT NULL DEFAULT 'raw', thread_id INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS window_terms ( window_id INTEGER NOT NULL REFERENCES windows(id) ON DELETE CASCADE, term TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS window_files ( window_id INTEGER NOT NULL REFERENCES windows(id) ON DELETE CASCADE, file_path TEXT NOT NULL);
CREATE VIRTUAL TABLE IF NOT EXISTS window_fts USING fts5(summary, content='windows', content_rowid='id', tokenize='porter unicode61');
CREATE VIRTUAL TABLE IF NOT EXISTS window_fts_trigram USING fts5(summary, content='windows', content_rowid='id', tokenize='trigram');
CREATE TABLE IF NOT EXISTS local_operator ( secret_ref TEXT PRIMARY KEY, operator_uuid TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 0, display_hint TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
-- The authenticated setup-wizard user is the immutable first remote owner.
-- Its enrollment bearer is not itself a write grant: the grant becomes active
-- only after CSR signing binds it to a unique, durably rostered mTLS serial.
CREATE TABLE IF NOT EXISTS remote_first_user (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  principal TEXT NOT NULL UNIQUE,
  created_at INTEGER NOT NULL CHECK(created_at>=0)
);
CREATE TABLE IF NOT EXISTS remote_client_grants (
  bearer_sha256 TEXT PRIMARY KEY CHECK(length(bearer_sha256)=64),
  principal TEXT NOT NULL,
  tier TEXT NOT NULL CHECK(tier IN ('data','full')),
  cert_serial TEXT UNIQUE,
  created_at INTEGER NOT NULL CHECK(created_at>=0),
  bound_at INTEGER CHECK(bound_at IS NULL OR bound_at>=created_at),
  CHECK((cert_serial IS NULL)=(bound_at IS NULL))
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_remote_client_grants_one_pending
  ON remote_client_grants(principal) WHERE cert_serial IS NULL;
CREATE TABLE IF NOT EXISTS project_clones ( clone_path TEXT PRIMARY KEY, project_uuid TEXT NOT NULL, canonical_url TEXT NOT NULL DEFAULT '', origin_url TEXT NOT NULL DEFAULT '', upstream_url TEXT NOT NULL DEFAULT '', last_seen_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS branch_ownership ( id INTEGER PRIMARY KEY AUTOINCREMENT, repo_path TEXT NOT NULL, branch_name TEXT NOT NULL, session_id TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(repo_path, branch_name));
CREATE TABLE IF NOT EXISTS tool_local_availability ( tool_uuid TEXT PRIMARY KEY, usable INTEGER NOT NULL DEFAULT 0, binary_path TEXT NOT NULL DEFAULT '', checked_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS working_profile_observations_local ( id INTEGER PRIMARY KEY AUTOINCREMENT, working_profile_key TEXT NOT NULL, session_id TEXT NOT NULL DEFAULT '', signal TEXT NOT NULL, payload_json TEXT NOT NULL DEFAULT '{}', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS working_profile_state_local ( working_profile_key TEXT PRIMARY KEY, score REAL NOT NULL DEFAULT 0, observation_count INTEGER NOT NULL DEFAULT 0, last_observation_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS working_profile_promotion_progress ( working_profile_key TEXT PRIMARY KEY, last_promoted_observation_id INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS memory_cognify_jobs ( id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL DEFAULT 'cognify_unit', memory_id INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL DEFAULT 3, last_error TEXT NOT NULL DEFAULT '', claimed_by TEXT NOT NULL DEFAULT '', claimed_at TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(kind, memory_id));
CREATE INDEX IF NOT EXISTS idx_mcj_status ON memory_cognify_jobs(status, id);
CREATE TABLE IF NOT EXISTS context_snapshots ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, memory_id INTEGER NOT NULL, relevance_score REAL NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS conv_tool_events ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, tool_name TEXT NOT NULL, tool_input TEXT NOT NULL DEFAULT '{}', tool_result TEXT NOT NULL DEFAULT '', result_bytes INTEGER NOT NULL DEFAULT 0, chain_id INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_cte_session ON conv_tool_events(session_id, id);
CREATE TABLE IF NOT EXISTS conv_tool_chains ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, event_id_first INTEGER NOT NULL DEFAULT 0, event_id_last INTEGER NOT NULL DEFAULT 0, tools TEXT NOT NULL DEFAULT '', stub TEXT NOT NULL DEFAULT '', raw_bytes INTEGER NOT NULL DEFAULT 0, stub_bytes INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL DEFAULT 'active', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_ctc_session ON conv_tool_chains(session_id, id);
CREATE TABLE IF NOT EXISTS conv_context_state ( session_id TEXT PRIMARY KEY, last_event_id INTEGER NOT NULL DEFAULT 0, chain_count INTEGER NOT NULL DEFAULT 0, event_count INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS payload_rewrite_state ( session_id TEXT PRIMARY KEY, payload_epoch INTEGER NOT NULL DEFAULT 0, compaction_epoch INTEGER NOT NULL DEFAULT 0, last_prefix_hash TEXT NOT NULL DEFAULT '', last_payload_tokens INTEGER NOT NULL DEFAULT 0, last_rewrite_at TEXT NOT NULL DEFAULT '', deferred_rewrite_count INTEGER NOT NULL DEFAULT 0, consecutive_deferred_count INTEGER NOT NULL DEFAULT 0, bytes_saved_pending INTEGER NOT NULL DEFAULT 0, rewrite_reason TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE TABLE IF NOT EXISTS guardrail_events ( id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL DEFAULT '', recorded_at TEXT NOT NULL DEFAULT (datetime('now')), tool_name TEXT NOT NULL DEFAULT '', overall_risk REAL NOT NULL DEFAULT 0.0, action_risk REAL NOT NULL DEFAULT 0.0, diff_risk REAL NOT NULL DEFAULT 0.0, drift_risk REAL NOT NULL DEFAULT 0.0, antipattern_similarity REAL NOT NULL DEFAULT 0.0, recommendation TEXT NOT NULL DEFAULT '', labels TEXT NOT NULL DEFAULT '', final_action TEXT NOT NULL DEFAULT '', explanation TEXT NOT NULL DEFAULT '', dry_run INTEGER NOT NULL DEFAULT 1);
CREATE INDEX IF NOT EXISTS idx_guardrail_events_session ON guardrail_events(session_id, recorded_at);
CREATE TABLE IF NOT EXISTS delegate_learnings ( id INTEGER PRIMARY KEY AUTOINCREMENT, created_at TEXT NOT NULL DEFAULT (datetime('now')), session_id TEXT NOT NULL DEFAULT '', role TEXT NOT NULL DEFAULT '', failure_mode TEXT NOT NULL DEFAULT 'success', lesson TEXT NOT NULL DEFAULT '', evidence_json TEXT NOT NULL DEFAULT '{}', confidence REAL NOT NULL DEFAULT 0.5, auto_applied INTEGER NOT NULL DEFAULT 1, review_status TEXT NOT NULL DEFAULT 'pending', reviewed_at TEXT, reviewer_notes TEXT);
CREATE INDEX IF NOT EXISTS idx_delegate_learnings_role ON delegate_learnings(role, confidence DESC);
CREATE INDEX IF NOT EXISTS idx_delegate_learnings_review ON delegate_learnings(review_status, created_at);
CREATE TABLE IF NOT EXISTS interaction_events ( id INTEGER PRIMARY KEY AUTOINCREMENT, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')), session_id TEXT NOT NULL DEFAULT '', event_type TEXT NOT NULL, actor TEXT NOT NULL DEFAULT 'agent', payload TEXT NOT NULL DEFAULT '{}', outcome TEXT NOT NULL DEFAULT 'ok', reflected_at TEXT, promoted_at TEXT);
CREATE INDEX IF NOT EXISTS idx_ie_session ON interaction_events(session_id, created_at);
CREATE INDEX IF NOT EXISTS idx_ie_type ON interaction_events(event_type, created_at);
CREATE INDEX IF NOT EXISTS idx_ie_reflect ON interaction_events(reflected_at, created_at) WHERE reflected_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_ie_promote ON interaction_events(promoted_at, created_at) WHERE promoted_at IS NULL;
CREATE TABLE IF NOT EXISTS mcp_osv_cache ( ecosystem TEXT NOT NULL, name TEXT NOT NULL, version TEXT NOT NULL DEFAULT '', verdict TEXT NOT NULL, advisory_ids TEXT NOT NULL DEFAULT '', checked_at INTEGER NOT NULL, PRIMARY KEY (ecosystem, name, version));
CREATE INDEX IF NOT EXISTS idx_mcp_osv_cache_checked ON mcp_osv_cache(checked_at DESC);
-- Spec-driven roadmap dispatch loop runtime (from db1/roadmap_runtime.sql).
CREATE TABLE IF NOT EXISTS roadmap_dispatch ( id INTEGER PRIMARY KEY AUTOINCREMENT, roadmap_id TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'running', phase TEXT NOT NULL DEFAULT 'plan', token_profile TEXT NOT NULL DEFAULT 'balanced', require_slice_discussion INTEGER NOT NULL DEFAULT 1, budget_ceiling_tokens INTEGER NOT NULL DEFAULT 0, exit_reason TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(roadmap_id));
CREATE TABLE IF NOT EXISTS roadmap_unit_dispatch ( id INTEGER PRIMARY KEY AUTOINCREMENT, roadmap_id TEXT NOT NULL, unit_id TEXT NOT NULL, level TEXT NOT NULL DEFAULT 'task', state TEXT NOT NULL DEFAULT 'pending', tool_policy_mode TEXT NOT NULL DEFAULT 'execution', claimed_by TEXT NOT NULL DEFAULT '', claimed_at TEXT NOT NULL DEFAULT '', heartbeat_at TEXT NOT NULL DEFAULT '', verify_attempts INTEGER NOT NULL DEFAULT 0, dispatch_attempts INTEGER NOT NULL DEFAULT 0, worktree_path TEXT NOT NULL DEFAULT '', coord_job_id INTEGER NOT NULL DEFAULT 0, result TEXT NOT NULL DEFAULT '', error TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(roadmap_id, unit_id));
CREATE TABLE IF NOT EXISTS roadmap_milestone_lease ( milestone_id TEXT PRIMARY KEY, roadmap_id TEXT NOT NULL, lease_owner TEXT NOT NULL DEFAULT '', branch TEXT NOT NULL DEFAULT '', worktree_path TEXT NOT NULL DEFAULT '', heartbeat_at TEXT NOT NULL DEFAULT '', expires_at TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_roadmap_unit_dispatch_ready ON roadmap_unit_dispatch (roadmap_id, state);
-- Roundtable authoring pipeline ledger (docs/proposals/.../agent-roundtable-authoring-pipeline.md).
-- Namespaced, distinct from the autopilot `pipelines` table (#1). Durable across
-- restarts so a human gate can be answered later; the source of truth for the
-- done-bar and gate digest, not the bounded/non-durable /v1/runs store (#2).
CREATE TABLE IF NOT EXISTS roundtable_pipeline_runs ( id INTEGER PRIMARY KEY AUTOINCREMENT, idea TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'drafting', phase TEXT NOT NULL DEFAULT 'proposal', admission_class TEXT NOT NULL DEFAULT 'active', schema_version INTEGER NOT NULL DEFAULT 1, done_bar TEXT NOT NULL DEFAULT 'zero_blocking', brief TEXT NOT NULL DEFAULT '', gate_digest TEXT NOT NULL DEFAULT '', proposal_ref TEXT NOT NULL DEFAULT '', proposal_origin_hash TEXT NOT NULL DEFAULT '', diff_ref TEXT NOT NULL DEFAULT '', diff_origin_hash TEXT NOT NULL DEFAULT '', chunk_index_ref TEXT NOT NULL DEFAULT '', repo_root TEXT NOT NULL DEFAULT '', remote TEXT NOT NULL DEFAULT '', base_branch TEXT NOT NULL DEFAULT 'testing', head_branch TEXT NOT NULL DEFAULT '', workspace_id TEXT NOT NULL DEFAULT '', workspace_provider TEXT NOT NULL DEFAULT 'shared', worktree_path TEXT NOT NULL DEFAULT '', head_sha TEXT NOT NULL DEFAULT '', base_sha TEXT NOT NULL DEFAULT '', proposal_pr_number INTEGER NOT NULL DEFAULT 0, proposal_pr_url TEXT NOT NULL DEFAULT '', impl_pr_number INTEGER NOT NULL DEFAULT 0, impl_pr_url TEXT NOT NULL DEFAULT '', cost_scope TEXT NOT NULL DEFAULT 'roundtable_only', cost_source TEXT NOT NULL DEFAULT 'roundtable_result', cost_version INTEGER NOT NULL DEFAULT 1, proposal_phase_cost_usd REAL NOT NULL DEFAULT 0, impl_phase_cost_usd REAL NOT NULL DEFAULT 0, total_cost_usd REAL NOT NULL DEFAULT 0, accepted_question_count INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_rt_pipeline_runs_state ON roundtable_pipeline_runs(state, admission_class);
CREATE TABLE IF NOT EXISTS roundtable_pipeline_passes ( id INTEGER PRIMARY KEY AUTOINCREMENT, pipeline_id INTEGER NOT NULL REFERENCES roundtable_pipeline_runs(id) ON DELETE CASCADE, phase TEXT NOT NULL, mode TEXT NOT NULL, pass_no INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'open', artifact_hash TEXT NOT NULL DEFAULT '', converged INTEGER NOT NULL DEFAULT 0, envelope_valid INTEGER NOT NULL DEFAULT 0, blocking_count INTEGER NOT NULL DEFAULT 0, suggestion_count INTEGER NOT NULL DEFAULT 0, nit_count INTEGER NOT NULL DEFAULT 0, open_questions INTEGER NOT NULL DEFAULT 0, coverage_gaps INTEGER NOT NULL DEFAULT 0, items_round INTEGER NOT NULL DEFAULT 0, artifact_round INTEGER NOT NULL DEFAULT 0, best_round INTEGER NOT NULL DEFAULT 0, rounds_run INTEGER NOT NULL DEFAULT 0, cost_usd REAL NOT NULL DEFAULT 0, result_hash TEXT NOT NULL DEFAULT '', is_chunked INTEGER NOT NULL DEFAULT 0, chunk_total INTEGER NOT NULL DEFAULT 0, chunk_done INTEGER NOT NULL DEFAULT 0, synthesis_done INTEGER NOT NULL DEFAULT 0, chunk_group INTEGER NOT NULL DEFAULT 0, chunk_index INTEGER NOT NULL DEFAULT 0, answered_count INTEGER NOT NULL DEFAULT 0, chunk_offset INTEGER NOT NULL DEFAULT 0, chunk_len INTEGER NOT NULL DEFAULT 0, chunk_omitted INTEGER NOT NULL DEFAULT 0, chunk_over_budget INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(pipeline_id, phase, pass_no));
CREATE INDEX IF NOT EXISTS idx_rt_pipeline_passes_pipeline ON roundtable_pipeline_passes(pipeline_id, id);
CREATE TABLE IF NOT EXISTS roundtable_pipeline_attempts ( id INTEGER PRIMARY KEY AUTOINCREMENT, pass_id INTEGER NOT NULL REFERENCES roundtable_pipeline_passes(id) ON DELETE CASCADE, attempt_no INTEGER NOT NULL, run_id TEXT NOT NULL DEFAULT '', is_current INTEGER NOT NULL DEFAULT 1, capture_status TEXT NOT NULL DEFAULT 'pending', terminal_status TEXT NOT NULL DEFAULT '', parse_status TEXT NOT NULL DEFAULT '', envelope_valid INTEGER NOT NULL DEFAULT 0, items_truncated INTEGER NOT NULL DEFAULT 0, truncated INTEGER NOT NULL DEFAULT 0, degraded INTEGER NOT NULL DEFAULT 0, cost_capped INTEGER NOT NULL DEFAULT 0, deadline_hit INTEGER NOT NULL DEFAULT 0, cancelled INTEGER NOT NULL DEFAULT 0, lost_result INTEGER NOT NULL DEFAULT 0, result_hash TEXT NOT NULL DEFAULT '', result_snapshot TEXT NOT NULL DEFAULT '', cost_usd REAL NOT NULL DEFAULT 0, cost_known INTEGER NOT NULL DEFAULT 0, submitted_at TEXT NOT NULL DEFAULT (datetime('now')), terminal_at TEXT NOT NULL DEFAULT '', UNIQUE(pass_id, attempt_no));
CREATE INDEX IF NOT EXISTS idx_rt_pipeline_attempts_run ON roundtable_pipeline_attempts(run_id);
CREATE TABLE IF NOT EXISTS roundtable_pipeline_gates ( id INTEGER PRIMARY KEY AUTOINCREMENT, pipeline_id INTEGER NOT NULL REFERENCES roundtable_pipeline_runs(id) ON DELETE CASCADE, gate_no INTEGER NOT NULL, verdict TEXT NOT NULL DEFAULT '', reason TEXT NOT NULL DEFAULT '', actor TEXT NOT NULL DEFAULT '', pr_number INTEGER NOT NULL DEFAULT 0, expected_head_sha TEXT NOT NULL DEFAULT '', merge_sha TEXT NOT NULL DEFAULT '', merge_executor TEXT NOT NULL DEFAULT '', merge_command TEXT NOT NULL DEFAULT '', merge_output TEXT NOT NULL DEFAULT '', merge_exit_code INTEGER NOT NULL DEFAULT 0, resolved_at TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_rt_pipeline_gates_pipeline ON roundtable_pipeline_gates(pipeline_id, gate_no);

-- Workflow engine: development-lifecycle work items + audit + per-stage attempts.
-- (lifecycle_* names intentionally avoid the charter artifact/audit role words.)
CREATE TABLE IF NOT EXISTS lifecycle_work_item ( id INTEGER PRIMARY KEY AUTOINCREMENT, work_item_id TEXT NOT NULL UNIQUE, repo TEXT NOT NULL DEFAULT '', proposal_path TEXT NOT NULL DEFAULT '', workflow_name TEXT NOT NULL DEFAULT 'build', workflow_version TEXT NOT NULL DEFAULT '', current_stage TEXT NOT NULL DEFAULT '', state TEXT NOT NULL DEFAULT 'active', mode TEXT NOT NULL DEFAULT 'interactive', pause_reason TEXT NOT NULL DEFAULT '', paused_state TEXT NOT NULL DEFAULT '', content_hash TEXT NOT NULL DEFAULT '', pr_ref TEXT NOT NULL DEFAULT '', worktree TEXT NOT NULL DEFAULT '', submitter TEXT NOT NULL DEFAULT '', cum_cost_usd REAL NOT NULL DEFAULT 0, work_item_max_cost_usd REAL NOT NULL DEFAULT 0, override_count INTEGER NOT NULL DEFAULT 0, parent_id TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(repo, proposal_path));
CREATE TABLE IF NOT EXISTS lifecycle_event ( id INTEGER PRIMARY KEY AUTOINCREMENT, work_item_id TEXT NOT NULL, stage TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL DEFAULT '', actor TEXT NOT NULL DEFAULT '', detail TEXT NOT NULL DEFAULT '', content_hash TEXT NOT NULL DEFAULT '', cost_usd REAL NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_lifecycle_event_wi ON lifecycle_event(work_item_id);
CREATE INDEX IF NOT EXISTS idx_lwi_submitter_state_mode ON lifecycle_work_item(submitter, state, mode);
CREATE INDEX IF NOT EXISTS idx_lwi_submitter_created ON lifecycle_work_item(submitter, created_at);
CREATE TABLE IF NOT EXISTS lifecycle_stage_attempt ( work_item_id TEXT NOT NULL, stage TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (work_item_id, stage));
-- S2 primary-as-manager: interactive session <-> work-item binding (single-writer).
-- aimee_session_id is PK (one binding per session); the index on work_item_id lets
-- the bind path reject a second session binding the same work-item. enforce_stage
-- is stamped once at bind and is monotonic per work-item (never downgraded on re-bind).
CREATE TABLE IF NOT EXISTS workflow_binding ( aimee_session_id TEXT NOT NULL PRIMARY KEY, work_item_id TEXT NOT NULL, enforce_stage TEXT NOT NULL DEFAULT 'off', lease_expiry TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')));
CREATE INDEX IF NOT EXISTS idx_workflow_binding_wi ON workflow_binding(work_item_id);
CREATE TABLE IF NOT EXISTS harness_memory ( id INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL, name TEXT NOT NULL, type TEXT NOT NULL DEFAULT 'fact' CHECK (type IN ('fact','index','note','scratch')), description TEXT, body TEXT NOT NULL DEFAULT '', meta_json TEXT NOT NULL DEFAULT '{}', content_hash TEXT NOT NULL DEFAULT '', last_client TEXT NOT NULL DEFAULT '', source_session TEXT NOT NULL DEFAULT '', schema_version INTEGER NOT NULL DEFAULT 1, deleted_at TEXT DEFAULT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(project, name));
CREATE INDEX IF NOT EXISTS idx_harness_memory_project ON harness_memory(project, deleted_at);
-- Proposal 2 (db1/db2 memory): per-user structured memory store. db1 is per-user by
-- construction (aimee-server is 1:1 per user), so no tenancy column is needed. Mirrors the
-- db2 'memories' core columns + selector patterns so recall can query both stores uniformly.
CREATE TABLE IF NOT EXISTS user_memories ( id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL DEFAULT 'fact', tier TEXT NOT NULL DEFAULT 'L2', key TEXT NOT NULL, content TEXT NOT NULL DEFAULT '', confidence REAL NOT NULL DEFAULT 1.0, use_count INTEGER NOT NULL DEFAULT 0, last_used_at TEXT DEFAULT NULL, lifecycle_state TEXT NOT NULL DEFAULT 'active', valid_until TEXT DEFAULT NULL, source_session TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(kind, key));
CREATE INDEX IF NOT EXISTS idx_user_memories_kind ON user_memories(kind, lifecycle_state);
