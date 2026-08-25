-- schema_grants.sql: P1 hardened-tier runtime GRANTs (phase 3 of provisioning).
--
-- Applied AFTER schema_roles.sql (roles exist) AND schema.sql (tables exist), by
-- the migration/owner path — NEVER by the runtime service. Gives the non-owner,
-- NOBYPASSRLS runtime role DML on the tenant tables (RLS still constrains every
-- row), INSERT/SELECT-only on the WORM audit store, sequence usage, and EXECUTE on
-- the one context setter. Dev/single-owner deployments skip this file entirely
-- (they run no three-role split); it is a no-op-safe re-run on a hardened tier.

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE NOTICE 'schema_grants: aimee_kb_runtime absent (dev tier) — skipping grants';
    RETURN;
  END IF;

  -- DML on existing + future tables; DDL (owner) excluded.
  GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO aimee_kb_runtime;
  -- The primary maintenance barrier is owner-only even though the legacy grant
  -- bootstrap is intentionally broad.  Keep this revoke adjacent so reapplication
  -- can never restore a runtime mutation or observation path.
  REVOKE ALL ON TABLE kb_vault_control FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_startup_status() FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_vault_control_startup_status() TO aimee_kb_runtime;
  -- P7-reseal-c is migration-owner orchestration only. The broad bootstrap grant
  -- above must never make its operation ledger, staged wraps, outbox, or helper
  -- functions visible to the runtime role on either first apply or re-apply.
  REVOKE ALL ON TABLE kb_vault_rewrap_operation, kb_vault_rewrap_dek_stage,
    kb_vault_rewrap_check_stage, kb_vault_rewrap_worm FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_rewrap_worm_block(),
    org_vault_rewrap_pack_text(TEXT), org_vault_rewrap_pack_bytes(BYTEA),
    org_vault_rewrap_worm_append(TEXT,TEXT,TEXT,TEXT),
    org_vault_rewrap_begin(TEXT,TEXT,TEXT,BIGINT,BIGINT),
    org_vault_rewrap_record_prepared(TEXT,BIGINT,BYTEA,BYTEA),
    org_vault_rewrap_assert_live(TEXT,BIGINT,BOOLEAN),
    org_vault_rewrap_status(TEXT),
    org_vault_rewrap_snapshot(TEXT),
    org_vault_rewrap_secret_page(TEXT,BIGINT,BIGINT,INTEGER),
    org_vault_rewrap_check_page(TEXT,BIGINT,BYTEA,INTEGER),
    org_vault_rewrap_stage_dek(TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,BIGINT,BYTEA,BYTEA),
    org_vault_rewrap_stage_check(TEXT,BIGINT,TEXT,BYTEA,BYTEA),
    org_vault_rewrap_digests(TEXT), org_vault_rewrap_inventory_summary(TEXT,BIGINT),
    org_vault_rewrap_stage_finish(TEXT,BIGINT,BIGINT,BIGINT,BYTEA),
    org_vault_rewrap_mark_committing(TEXT,BIGINT),
    org_vault_rewrap_mark_resealed(TEXT,BIGINT,BYTEA),
    org_vault_rewrap_promote(TEXT,BIGINT),
    org_vault_rewrap_verify_summary(TEXT,BIGINT),
    org_vault_rewrap_verify_secret_page(TEXT,BIGINT,BIGINT,INTEGER),
    org_vault_rewrap_verify_check_page(TEXT,BIGINT,BYTEA,INTEGER),
    org_vault_rewrap_complete(TEXT,BIGINT,BYTEA,BYTEA,BYTEA),
    org_vault_rewrap_abort(TEXT,BIGINT,TEXT),
    org_vault_rewrap_recovery_required(TEXT,BIGINT,TEXT)
    FROM aimee_kb_runtime;
  -- P7-witness: the kb's cadence, boot check, and release gate run on the runtime
  -- connection, so it needs READ on the (non-secret, log-exported) evidence and
  -- EXECUTE on the cadence functions — but must never forge evidence or touch the
  -- control row. Runs last, so it is the authority; it matches the DO block in
  -- schema.sql. Read-only on the evidence tables (no INSERT -> no forge; WORM blocks
  -- UPDATE/DELETE); the emit_cursor is also read-only (writes go via the definer).
  REVOKE ALL ON TABLE kb_vault_witness_shard, kb_vault_witness_log,
    kb_vault_witness_checkpoint, kb_vault_witness_emit_cursor FROM aimee_kb_runtime;
  GRANT SELECT ON TABLE kb_vault_witness_shard, kb_vault_witness_log,
    kb_vault_witness_checkpoint, kb_vault_witness_emit_cursor TO aimee_kb_runtime;
  -- Internal / definer-nested helpers stay invisible (append is nested in the
  -- source-ledger definers; verify_shard nested in the leaf scan).
  REVOKE ALL ON FUNCTION org_vault_witness_worm_block(),
    org_vault_witness_genesis(TEXT,TEXT),
    org_vault_witness_record_digest(SMALLINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BYTEA,BOOLEAN,BYTEA,BYTEA,BIGINT,BIGINT,BIGINT),
    org_vault_witness_append(SMALLINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BYTEA,BOOLEAN,BYTEA),
    org_vault_witness_verify_shard(TEXT,TEXT)
    FROM aimee_kb_runtime;
  -- The cadence/boot/gate call these directly.
  GRANT EXECUTE ON FUNCTION org_vault_witness_control_fence(),
    org_vault_witness_checkpoint_leaves(),
    org_vault_witness_checkpoint_persist(BIGINT,BYTEA,BOOLEAN,BYTEA,BIGINT,BYTEA,BYTEA,BYTEA,SMALLINT,INTEGER,BYTEA,BIGINT),
    org_vault_witness_emit_pending(),
    org_vault_witness_emit_batch(TEXT,TEXT,BIGINT,INTEGER),
    org_vault_witness_emit_checkpoints(BIGINT,INTEGER),
    org_vault_witness_emit_advance(SMALLINT,TEXT,TEXT,BIGINT)
    TO aimee_kb_runtime;
  GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO aimee_kb_runtime;
  ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner IN SCHEMA public
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_kb_runtime;
  ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner IN SCHEMA public
    GRANT USAGE, SELECT ON SEQUENCES TO aimee_kb_runtime;

  -- Explicit tenant-table grants (in case ALL TABLES ran before these existed).
  GRANT SELECT, INSERT, UPDATE, DELETE ON
    kb_team, kb_project, kb_team_membership, kb_project_membership,
    kb_admin_grant, kb_oidc_jwks TO aimee_kb_runtime;

  -- WORM audit store: runtime may INSERT/SELECT only, never UPDATE/DELETE.
  REVOKE UPDATE, DELETE, TRUNCATE ON kb_audit_event FROM aimee_kb_runtime;
  GRANT INSERT, SELECT ON kb_audit_event TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION memory_mutation_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT)
    TO aimee_kb_runtime;

  -- Rejections are durable state, not disposable queue rows. Runtime may create
  -- a tombstone and may deactivate it during an explicitly authorized restore,
  -- but it must never erase the review record outright.
  REVOKE DELETE, TRUNCATE ON memory_rejection_tombstones FROM aimee_kb_runtime;
  GRANT SELECT, INSERT, UPDATE ON memory_rejection_tombstones TO aimee_kb_runtime;

  -- set_tenant_context is the ONLY runtime-usable tenant-GUC setter; EXECUTE to
  -- the runtime role only, never PUBLIC (N4).
  REVOKE ALL ON FUNCTION set_tenant_context(TEXT, BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION set_tenant_context(TEXT, BIGINT) TO aimee_kb_runtime;
  REVOKE ALL ON FUNCTION set_maintenance_context(TEXT, TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION set_maintenance_context(TEXT, TEXT) TO aimee_kb_runtime;
  REVOKE ALL ON FUNCTION kb_content_project_attribute(TEXT, BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION kb_content_project_attribute(TEXT, BIGINT) TO aimee_kb_runtime;

  -- P5-A authoritative registry: runtime reaches state only through audited,
  -- bounded definer APIs; direct reads and writes are unavailable.
  REVOKE ALL ON kb_server_registry, kb_cert_revocation_generation FROM aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_server_registry_pending(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,INT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_server_registry_finalize(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_server_registry_heartbeat(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_server_registry_client_match(TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_server_registry_list(BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_server_registry_snapshot(BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION kb_management_status_lookup(TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;

  -- P3a cost attribution. The ledger, rollup, and price tables are WRITTEN ONLY by
  -- the SECURITY DEFINER metering functions (owned by aimee_kb_owner, which bypasses
  -- ENABLE-not-FORCE RLS). Runtime therefore gets SELECT (RLS-filtered: admin OR
  -- team-lead) but its direct write grant from the ALL TABLES line above is REVOKED,
  -- so a compromised runtime session cannot forge or mutate cost rows out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_model_pricing, org_model_pricing_current, org_token_audit, org_spend_rollup
    FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_model_pricing, org_model_pricing_current, org_token_audit, org_spend_rollup
    TO aimee_kb_runtime;
  -- kb_team_lead is an admin-written grant (RLS gates writes to admins), same posture
  -- as kb_admin_grant: runtime holds DML, RLS constrains it.
  GRANT SELECT, INSERT, UPDATE, DELETE ON kb_team_lead TO aimee_kb_runtime;

  -- kb_write_tier_grant authorizes per-user /v1 writes. Runtime READS through
  -- RLS, but the definer functions are the ONLY write path: a grant change must
  -- carry a WORM audit row, and kb_audit_worm_append is not granted to runtime
  -- (so runtime cannot forge audit rows). Withholding INSERT/UPDATE here is what
  -- makes the audit inseparable from the change rather than merely conventional.
  -- DELETE stays withheld too: a grant is revoked, never erased.
  GRANT SELECT ON kb_write_tier_grant TO aimee_kb_runtime;
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON kb_write_tier_grant FROM aimee_kb_runtime;

  -- Identity-token authority state is authority-owned, exactly like the
  -- management and read intents it sits beside: ordinary runtime must not be
  -- able to read a pending mint or the immutable key-use record, even though RLS
  -- would already return no rows. The blanket "GRANT ... ON ALL TABLES" above
  -- reaches these tables, so the grant has to be taken back explicitly.
  REVOKE ALL ON kb_management_identity_intent,
    kb_management_identity_key_use_intent FROM aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION
    kb_write_tier_grant_set(TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  -- The reporting wrapper delegates to the function above for every decision, so it
  -- needs no privilege the runtime does not already hold.
  GRANT EXECUTE ON FUNCTION
    kb_write_tier_grant_set_reporting(TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION
    kb_write_tier_grant_revoke(TEXT,BIGINT,TEXT) TO aimee_kb_runtime;

  -- The metering functions are the ONLY write path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_pricing_add_version(TEXT,TEXT,NUMERIC,NUMERIC,NUMERIC,NUMERIC) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_pricing_current_version(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_token_estimate_cost(TEXT,BIGINT,BIGINT,BIGINT,BIGINT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_token_audit_start(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_token_audit_settle(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BIGINT,BIGINT,NUMERIC,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_pricing_add_version(TEXT,TEXT,NUMERIC,NUMERIC,NUMERIC,NUMERIC) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_pricing_current_version(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_token_estimate_cost(TEXT,BIGINT,BIGINT,BIGINT,BIGINT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_token_audit_start(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_token_audit_settle(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BIGINT,BIGINT,NUMERIC,TEXT) TO aimee_kb_runtime;

  -- P3b org spend reporting. The read-only aggregation definer enforces the admin/lead
  -- predicate INTERNALLY (it IS the authz gate — SECURITY DEFINER bypasses RLS), so it
  -- is the single authorized reporting path over org_spend_rollup; EXECUTE to runtime,
  -- never PUBLIC.
  REVOKE ALL ON FUNCTION org_spend_query(BIGINT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_spend_query(BIGINT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;

  -- P10 kb credential vault. The ciphertext store is WRITTEN ONLY by the SECURITY
  -- DEFINER vault functions (owned by aimee_kb_owner, which bypasses RLS for its own
  -- internal version scan). Runtime therefore gets SELECT (RLS-filtered: own-team rows
  -- / admin) but its direct write grant from the ALL TABLES line above is REVOKED, so a
  -- compromised runtime session cannot forge or mutate vault rows out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_vault_salt, org_vault_secret, org_vault_current FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_vault_salt, org_vault_secret, org_vault_current TO aimee_kb_runtime;
  REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON org_vault_rotation FROM aimee_kb_runtime;
  -- The vault definer functions are the ONLY write/read-through path; EXECUTE to
  -- runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_vault_salt_ensure(TEXT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_salt_read(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_kek_check_read(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_kek_check_set(TEXT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_put(TEXT,BIGINT,TEXT,TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_get_current(TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_has(TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_list(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_list_principals() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_delete(TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_current_wraps(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rewrap(TEXT,TEXT,TEXT,BIGINT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_authorized(TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,BIGINT,BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_stage(TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_transition(TEXT,BIGINT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_finalize(TEXT,BIGINT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_get(BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_claim(TEXT,BIGINT,TEXT,TEXT,INTEGER) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_heartbeat(TEXT,BIGINT,TEXT,BIGINT,INTEGER) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_release(TEXT,BIGINT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_checkpoint_old_ref(TEXT,BIGINT,TEXT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_stage_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,BYTEA,BYTEA,BYTEA,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_probe_admit(TEXT,BIGINT,TEXT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_transition_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_fail_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_remediate(TEXT,BIGINT,TEXT,BIGINT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_key_use_candidate(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_key_use_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_vault_salt_ensure(TEXT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_salt_read(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_kek_check_read(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_kek_check_set(TEXT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_put(TEXT,BIGINT,TEXT,TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_get_current(TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_has(TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_list(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_list_principals() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_delete(TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_current_wraps(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rewrap(TEXT,TEXT,TEXT,BIGINT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,BIGINT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_stage(TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_transition(TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_finalize(TEXT,BIGINT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_get(BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_claim(TEXT,BIGINT,TEXT,TEXT,INTEGER) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_heartbeat(TEXT,BIGINT,TEXT,BIGINT,INTEGER) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_release(TEXT,BIGINT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_checkpoint_old_ref(TEXT,BIGINT,TEXT,BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_stage_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,BYTEA,BYTEA,BYTEA,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_probe_admit(TEXT,BIGINT,TEXT,BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_transition_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_fail_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_remediate(TEXT,BIGINT,TEXT,BIGINT,BIGINT,TEXT) TO aimee_kb_runtime;
  REVOKE SELECT,INSERT,UPDATE,DELETE,TRUNCATE ON org_vault_key_use_intent FROM aimee_kb_runtime;
  REVOKE ALL ON TABLE kb_vault_control FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_startup_status() FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_vault_control_startup_status() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_key_use_candidate(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_key_use_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA) TO aimee_kb_runtime;

  -- P2a org model catalog + entitlement. The catalog is admin-managed and read/written
  -- EXCLUSIVELY through the SECURITY DEFINER functions (owned by aimee_kb_owner, which
  -- bypasses ENABLE-not-FORCE RLS). Runtime gets NO direct catalog access AT ALL — not
  -- even SELECT — so every catalog read funnels through org_catalog_entitled(); its
  -- direct write grant from the ALL TABLES line above is REVOKED too. Entitlement direct
  -- SELECT stays (RLS-filtered: own-team OR admin) for defense-in-depth, but its writes
  -- are REVOKED (only the definer functions may mutate it).
  REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON org_model_catalog FROM aimee_kb_runtime;
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON org_model_entitlement FROM aimee_kb_runtime;
  GRANT SELECT ON org_model_entitlement TO aimee_kb_runtime;
  -- The catalog CRUD + entitled-read functions are the ONLY access path; EXECUTE to
  -- runtime, never PUBLIC. kb_audit_worm_append is intentionally NOT granted to runtime
  -- (only the owner-run definer mutations call it) so runtime cannot forge audit rows.
  REVOKE ALL ON FUNCTION kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_entitled() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,BOOLEAN) FROM PUBLIC;
  -- P6c-catalog: the bedrock companion write path is the ONLY way to land a provider=
  -- 'bedrock' row (the plain org_catalog_upsert fail-closes on it). Runtime keeps NO direct
  -- catalog access; the new bedrock_* columns are therefore reachable ONLY via the definer
  -- (org_catalog_entitled()'s projection is UNCHANGED, so account/ARNs/region never leak to
  -- a tenant read). org_bedrock_adapter_supported is a pure predicate (no table access) —
  -- left PUBLIC-callable (harmless), plus an explicit runtime grant for clarity.
  REVOKE ALL ON FUNCTION org_catalog_bedrock_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT[],TEXT[],TEXT,BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_bedrock_target(BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_remove(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_model_entitle(TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_model_unentitle(TEXT,BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_catalog_entitled() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_bedrock_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT[],TEXT[],TEXT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_bedrock_target(BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_bedrock_adapter_supported(TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_remove(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_model_entitle(TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_model_unentitle(TEXT,BIGINT) TO aimee_kb_runtime;

  -- P4a budget reservation core. The four budget tables are WRITTEN ONLY by the SECURITY
  -- DEFINER reserve/settle/set functions (owned by aimee_kb_owner, which bypasses
  -- ENABLE-not-FORCE RLS). Runtime therefore gets SELECT (RLS-filtered: admin OR
  -- team-lead on config/counter, admin-only on reservation/alloc) but its direct write
  -- grant from the ALL TABLES line above is REVOKED, so a compromised runtime session
  -- cannot forge or mutate a reservation/counter out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_budget, org_budget_counter, org_budget_reservation, org_budget_reservation_alloc
    FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_budget, org_budget_counter, org_budget_reservation, org_budget_reservation_alloc
    TO aimee_kb_runtime;
  -- The budget functions are the ONLY write path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_budget_period_id(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_reserve(TEXT,TEXT,BIGINT,BIGINT,BIGINT,NUMERIC,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_settle(TEXT,TEXT,NUMERIC) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_settle_expired() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_heartbeat(TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_set(BIGINT,BIGINT,TEXT,NUMERIC,NUMERIC) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_show(BIGINT,BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_budget_period_id(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_reserve(TEXT,TEXT,BIGINT,BIGINT,BIGINT,NUMERIC,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_settle(TEXT,TEXT,NUMERIC) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_settle_expired() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_heartbeat(TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_set(BIGINT,BIGINT,TEXT,NUMERIC,NUMERIC) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_show(BIGINT,BIGINT) TO aimee_kb_runtime;

  -- P4b keyed fixed-window rate limiter. org_rate_policy + org_rate_window are WRITTEN
  -- ONLY by the SECURITY DEFINER check/set functions (owned by aimee_kb_owner, which
  -- bypasses ENABLE-not-FORCE RLS). Runtime gets SELECT on the policy config
  -- (RLS-filtered: admin OR team-lead), but its direct write grant from the ALL TABLES
  -- line is REVOKED. org_rate_window is a definer-only operational counter: runtime gets
  -- NO direct read or write, so a compromised runtime session cannot read or forge the
  -- shared window counters out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON org_rate_policy FROM aimee_kb_runtime;
  GRANT SELECT ON org_rate_policy TO aimee_kb_runtime;
  REVOKE ALL ON org_rate_window FROM aimee_kb_runtime;
  -- The rate functions are the ONLY write path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_rate_check(BIGINT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_rate_policy_set(TEXT,TEXT,INT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_rate_policy_show(TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_rate_check(BIGINT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_rate_policy_set(TEXT,TEXT,INT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_rate_policy_show(TEXT,TEXT) TO aimee_kb_runtime;

  -- P2b-a egress authority. Runtime has no direct access to the private binding or
  -- durable dispatch ledger; every state change is owner-scoped and fenced by a
  -- SECURITY DEFINER function. The admin-gated binding setter is intentionally
  -- executable by runtime because it enforces kb_principal_is_admin internally.
  REVOKE ALL ON org_egress_binding, org_egress_dispatch FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_egress_binding_set(BIGINT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_admit(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_dispatch_begin(TEXT,TEXT,TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_dispatch_heartbeat(BIGINT,TEXT,BIGINT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_dispatch_settle(BIGINT,TEXT,BIGINT,TEXT,INT,BIGINT,BIGINT,BIGINT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_recover(INT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_egress_binding_set(BIGINT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_admit(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_begin(TEXT,TEXT,TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_heartbeat(BIGINT,TEXT,BIGINT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_owner_guard(BIGINT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_settle(BIGINT,TEXT,BIGINT,TEXT,INT,BIGINT,BIGINT,BIGINT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_recover(INT) TO aimee_kb_runtime;

  -- Certificate renewal is a single SECURITY DEFINER lineage/grant/audit
  -- mutation. Runtime cannot invoke the underlying WORM appender directly.
  REVOKE ALL ON FUNCTION kb_enrollment_renew(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION kb_enrollment_renew(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;

  -- P9a telemetry export + content-free ingest. org_telemetry + org_telemetry_allowlist
  -- are WRITTEN ONLY by the SECURITY DEFINER ingest/allow functions (owned by
  -- aimee_kb_owner, which bypasses ENABLE-not-FORCE RLS). Runtime gets SELECT on
  -- org_telemetry (RLS-filtered: admin OR team-lead of the row's team) for defense in
  -- depth, but its direct write grant from the ALL TABLES line is REVOKED. The allowlist
  -- is admin-managed: runtime gets NO direct access AT ALL (read via org_telemetry_allow_show,
  -- which enforces the admin gate). /v1/metrics never reads org_telemetry — it reads the
  -- authoritative rollup tables via org_metrics_snapshot (the write-only-target invariant).
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON org_telemetry FROM aimee_kb_runtime;
  GRANT SELECT ON org_telemetry TO aimee_kb_runtime;
  REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON org_telemetry_allowlist FROM aimee_kb_runtime;
  -- The telemetry functions are the ONLY access path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_telemetry_ingest(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,NUMERIC,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_telemetry_allow(TEXT,TEXT[],BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_telemetry_allow_show() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_metrics_snapshot() FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_telemetry_ingest(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,NUMERIC,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_telemetry_allow(TEXT,TEXT[],BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_telemetry_allow_show() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_metrics_snapshot() TO aimee_kb_runtime;
END
$$;

-- P7-reseal-d3a dedicated operator-status authority.  The login role cannot
-- inherit this capability implicitly; the runtime must prove its posture and
-- execute the fixed SET ROLE before using the single discovery facade.
DO $p7_d3a_orchestrator_grants$
DECLARE
  role_name TEXT;
  schema_name TEXT;
  fn RECORD;
  fn_signature TEXT;
  orchestrator_functions CONSTANT TEXT[] := ARRAY[
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_dispatch(text,text)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(text,text,text,bigint,bigint)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_active()',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed(text,text,text)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed_active(text,text)',
    'aimee_kb_vault_orchestrator_api.org_vault_current_check_page(bytea,integer)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_open_completed(text,text,text,bigint,bigint,bytea,bytea,bytea)',
    'aimee_kb_vault_orchestrator_api.org_vault_open_idle(text,text,bigint,bigint,bigint)',
    'aimee_kb_vault_orchestrator_api.org_vault_open_event(text)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_snapshot(text)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_record_prepared(text,bigint,bytea,bytea)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_secret_page(text,bigint,bigint,integer)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_check_page(text,bigint,bytea,integer)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_stage_dek(text,bigint,bigint,text,text,text,bigint,bytea,bytea)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_stage_check(text,bigint,text,bytea,bytea)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_inventory_summary(text,bigint)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_stage_finish(text,bigint,bigint,bigint,bytea)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_mark_committing(text,bigint)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_mark_resealed(text,bigint,bytea)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_promote(text,bigint)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_abort(text,bigint,text)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_recovery_required(text,bigint,text)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_verify_summary(text,bigint)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_verify_secret_page(text,bigint,bigint,integer)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_verify_check_page(text,bigint,bytea,integer)',
    'aimee_kb_vault_orchestrator_api.org_vault_rewrap_complete(text,bigint,bytea,bytea,bytea)'
  ];
  edge RECORD;
  owned RECORD;
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_vault_orchestrator') THEN
    CREATE ROLE aimee_kb_vault_orchestrator NOLOGIN NOINHERIT NOBYPASSRLS
      NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_vault_orchestrator_login') THEN
    CREATE ROLE aimee_kb_vault_orchestrator_login LOGIN NOINHERIT NOBYPASSRLS
      NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
  END IF;
  ALTER ROLE aimee_kb_vault_orchestrator NOLOGIN NOINHERIT NOBYPASSRLS
    NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
  ALTER ROLE aimee_kb_vault_orchestrator_login LOGIN NOINHERIT NOBYPASSRLS
    NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;

  -- Ownership would bypass ACL cleanup.  Repair any drift before reducing both
  -- principals to the same database CONNECT-only posture.
  IF (SELECT owner.rolname FROM pg_catalog.pg_database d
       JOIN pg_catalog.pg_roles owner ON owner.oid=d.datdba
      WHERE d.datname=current_database()) IN
       ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login') THEN
    EXECUTE format('ALTER DATABASE %I OWNER TO aimee_kb_owner',current_database());
  END IF;
  EXECUTE format('REVOKE ALL ON DATABASE %I FROM aimee_kb_vault_orchestrator, aimee_kb_vault_orchestrator_login',
                 current_database());
  EXECUTE format('REVOKE CREATE,TEMPORARY ON DATABASE %I FROM PUBLIC',current_database());
  EXECUTE format('GRANT CONNECT ON DATABASE %I TO aimee_kb_vault_orchestrator, aimee_kb_vault_orchestrator_login',
                 current_database());
  -- Strip latent direct grants from every schema, including pg_catalog. These
  -- REVOKEs affect only the dedicated roles; PUBLIC catalog access is untouched.
  -- NOINHERIT does not make such grants harmless: they would become usable if a
  -- future search_path or role transition changed.
  FOR schema_name IN
    SELECT n.nspname FROM pg_catalog.pg_namespace n
  LOOP
    IF (SELECT owner.rolname FROM pg_catalog.pg_namespace n
         JOIN pg_catalog.pg_roles owner ON owner.oid=n.nspowner
        WHERE n.nspname=schema_name) IN
         ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login') THEN
      EXECUTE format('ALTER SCHEMA %I OWNER TO aimee_kb_owner',schema_name);
    END IF;
    EXECUTE format('REVOKE ALL ON SCHEMA %I FROM aimee_kb_vault_orchestrator, aimee_kb_vault_orchestrator_login',schema_name);
    EXECUTE format('REVOKE ALL ON ALL TABLES IN SCHEMA %I FROM aimee_kb_vault_orchestrator, aimee_kb_vault_orchestrator_login',schema_name);
    EXECUTE format('REVOKE ALL ON ALL SEQUENCES IN SCHEMA %I FROM aimee_kb_vault_orchestrator, aimee_kb_vault_orchestrator_login',schema_name);
    EXECUTE format('REVOKE ALL ON ALL FUNCTIONS IN SCHEMA %I FROM aimee_kb_vault_orchestrator, aimee_kb_vault_orchestrator_login',schema_name);
  END LOOP;
  FOR owned IN
    SELECT c.oid::pg_catalog.regclass AS object_name,c.relkind
      FROM pg_catalog.pg_class c
      JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
      JOIN pg_catalog.pg_roles owner ON owner.oid=c.relowner
     WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema'
       AND owner.rolname IN
         ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')
       AND c.relkind IN ('r','p','f','S','v','m','c')
  LOOP
    IF owned.relkind='S' THEN
      EXECUTE format('ALTER SEQUENCE %s OWNER TO aimee_kb_owner',owned.object_name);
    ELSIF owned.relkind='v' THEN
      EXECUTE format('ALTER VIEW %s OWNER TO aimee_kb_owner',owned.object_name);
    ELSIF owned.relkind='m' THEN
      EXECUTE format('ALTER MATERIALIZED VIEW %s OWNER TO aimee_kb_owner',owned.object_name);
    ELSIF owned.relkind='c' THEN
      EXECUTE format('ALTER TYPE %s OWNER TO aimee_kb_owner',owned.object_name);
    ELSE
      EXECUTE format('ALTER TABLE %s OWNER TO aimee_kb_owner',owned.object_name);
    END IF;
  END LOOP;
  FOR owned IN
    SELECT p.oid::pg_catalog.regprocedure AS object_name,p.prokind
      FROM pg_catalog.pg_proc p
      JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
      JOIN pg_catalog.pg_roles owner ON owner.oid=p.proowner
     WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema'
       AND p.prokind IN ('f','w','p') AND owner.rolname IN
         ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')
  LOOP
    IF owned.prokind='p' THEN
      EXECUTE format('ALTER PROCEDURE %s OWNER TO aimee_kb_owner',owned.object_name);
    ELSE
      EXECUTE format('ALTER FUNCTION %s OWNER TO aimee_kb_owner',owned.object_name);
    END IF;
  END LOOP;
  -- Index ownership follows its table.  Any remaining class/procedure kind is
  -- unsupported drift and must stop provisioning rather than retain authority.
  IF EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
       JOIN pg_catalog.pg_roles owner ON owner.oid=c.relowner
        WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema'
          AND owner.rolname IN
            ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')) OR
     EXISTS (
       SELECT 1 FROM pg_catalog.pg_proc p
       JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       JOIN pg_catalog.pg_roles owner ON owner.oid=p.proowner
        WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema'
          AND owner.rolname IN
            ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')) THEN
    RAISE EXCEPTION 'P7_D3A_UNSUPPORTED_OWNERSHIP_DRIFT' USING ERRCODE='55000';
  END IF;
  REVOKE USAGE ON SCHEMA public FROM PUBLIC;
  FOR edge IN
    SELECT granted.rolname AS granted_role,member.rolname AS member_role
      FROM pg_catalog.pg_auth_members membership
      JOIN pg_catalog.pg_roles granted ON granted.oid=membership.roleid
      JOIN pg_catalog.pg_roles member ON member.oid=membership.member
     WHERE member.rolname IN
       ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')
        OR granted.rolname IN
       ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')
  LOOP
    EXECUTE format('REVOKE %I FROM %I CASCADE',edge.granted_role,edge.member_role);
  END LOOP;
  GRANT aimee_kb_vault_orchestrator TO aimee_kb_vault_orchestrator_login;

  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_vault_orchestrator;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_vault_orchestrator;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_vault_orchestrator;
  REVOKE ALL ON SCHEMA public FROM aimee_kb_vault_orchestrator;

  -- The private schema makes schema USAGE part of the one-function capability;
  -- PUBLIC, the authenticator, and unrelated runtimes cannot resolve anything
  -- in it.  Pin both ownership and future-function defaults to the migration
  -- owner so a re-apply cannot reopen EXECUTE through PostgreSQL's PUBLIC
  -- function default.
  ALTER SCHEMA aimee_kb_vault_orchestrator_api OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_vault_control OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_vault_rewrap_operation OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_vault_rewrap_worm OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_vault_open_event OWNER TO aimee_kb_owner;
  ALTER FUNCTION public.org_vault_open_event_worm_block() OWNER TO aimee_kb_owner;
  FOR fn IN
    SELECT p.oid::regprocedure AS signature
      FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
     WHERE n.nspname='aimee_kb_vault_orchestrator_api'
  LOOP
    EXECUTE format('ALTER FUNCTION %s OWNER TO aimee_kb_owner',fn.signature);
  END LOOP;
  -- The SECURITY DEFINER resolves only explicitly-qualified public relations.
  -- PUBLIC schema usage is revoked globally above, so grant the function owner
  -- the resolution privilege it needs without exposing it to the capability.
  GRANT USAGE ON SCHEMA public TO aimee_kb_owner;
  -- PostgreSQL row-locking SELECTs also require UPDATE authority.
  GRANT SELECT,INSERT,UPDATE,DELETE ON TABLE public.kb_vault_control,
    public.kb_vault_rewrap_operation,public.kb_vault_rewrap_dek_stage,
    public.kb_vault_rewrap_check_stage,public.kb_vault_rewrap_worm,
    public.kb_vault_open_event TO aimee_kb_owner;
  GRANT SELECT ON TABLE public.org_vault_rotation,public.org_vault_salt,
    public.org_vault_secret TO aimee_kb_owner;
  GRANT EXECUTE ON FUNCTION public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
    TO aimee_kb_owner;
  REVOKE ALL ON SCHEMA aimee_kb_vault_orchestrator_api FROM PUBLIC;
  REVOKE ALL ON SCHEMA aimee_kb_vault_orchestrator_api
    FROM aimee_kb_vault_orchestrator_login,aimee_kb_runtime;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA aimee_kb_vault_orchestrator_api FROM PUBLIC;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA aimee_kb_vault_orchestrator_api
    FROM aimee_kb_vault_orchestrator_login,aimee_kb_runtime,
         aimee_kb_vault_orchestrator;
  -- A per-schema REVOKE cannot override PostgreSQL's global PUBLIC EXECUTE
  -- default.  Close this owner's future-function default globally; application
  -- execution grants in this provisioning file are explicit.
  ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner
    REVOKE EXECUTE ON FUNCTIONS FROM PUBLIC;
  GRANT USAGE ON SCHEMA aimee_kb_vault_orchestrator_api
    TO aimee_kb_vault_orchestrator;

  REVOKE ALL ON TABLE public.kb_vault_control,
    public.kb_vault_rewrap_operation,public.kb_vault_rewrap_dek_stage,
    public.kb_vault_rewrap_check_stage,public.kb_vault_rewrap_worm,
    public.kb_vault_open_event FROM PUBLIC;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN
    REVOKE ALL ON TABLE public.kb_vault_control,
      public.kb_vault_rewrap_operation,public.kb_vault_rewrap_dek_stage,
      public.kb_vault_rewrap_check_stage,public.kb_vault_rewrap_worm,
      public.kb_vault_open_event FROM aimee_kb_runtime;
  END IF;

  FOR fn IN
    SELECT p.oid::regprocedure AS signature
      FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
     WHERE n.nspname='public' AND p.proname LIKE 'org_vault_rewrap_%'
  LOOP
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC',fn.signature);
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN
      EXECUTE format('REVOKE ALL ON FUNCTION %s FROM aimee_kb_runtime',fn.signature);
    END IF;
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO aimee_kb_owner',fn.signature);
  END LOOP;
  IF (SELECT pg_catalog.count(*) FROM pg_catalog.pg_proc p
        JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       WHERE n.nspname='aimee_kb_vault_orchestrator_api') <>
       pg_catalog.cardinality(orchestrator_functions) OR
     EXISTS (SELECT 1 FROM pg_catalog.pg_proc p
        JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       WHERE n.nspname='aimee_kb_vault_orchestrator_api'
         AND (p.oid::pg_catalog.regprocedure)::TEXT <> ALL(orchestrator_functions)) THEN
    RAISE EXCEPTION 'P7_D3B_ORCHESTRATOR_CAPABILITY_DRIFT' USING ERRCODE='55000';
  END IF;
  FOREACH fn_signature IN ARRAY orchestrator_functions LOOP
    IF pg_catalog.to_regprocedure(fn_signature) IS NULL THEN
      RAISE EXCEPTION 'P7_D3B_ORCHESTRATOR_CAPABILITY_MISSING' USING ERRCODE='55000';
    END IF;
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO aimee_kb_vault_orchestrator',fn_signature);
  END LOOP;
END
$p7_d3a_orchestrator_grants$;

-- P5-C2d dedicated online token authority.  The LOGIN runtime receives only
-- four fixed entry points.  A separate non-login function owner crosses FORCE
-- RLS with the exact read/lock/insert closure needed by those functions; ordinary kb,
-- publisher, provisioner, status and migration roles receive no authority.
DO $$
DECLARE role_name TEXT;
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles
      WHERE rolname='aimee_kb_token_authority_definer') OR
     NOT EXISTS (SELECT 1 FROM pg_roles
      WHERE rolname='aimee_kb_token_authority_runtime') OR
     NOT EXISTS (SELECT 1 FROM pg_roles
      WHERE rolname='aimee_kb_token_authority_store_owner') THEN
    RAISE EXCEPTION 'management token authority roles are required';
  END IF;

  ALTER TABLE public.kb_management_token_key_use_intent OWNER TO aimee_kb_token_authority_store_owner;
  ALTER TABLE public.kb_management_read_intent OWNER TO aimee_kb_token_authority_store_owner;
  ALTER TABLE public.kb_management_read_key_use OWNER TO aimee_kb_token_authority_store_owner;
  EXECUTE 'ALTER FUNCTION public.kb_management_token_key_use_worm_guard() OWNER TO aimee_kb_token_authority_store_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_transition_guard() OWNER TO aimee_kb_token_authority_store_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_worm_guard() OWNER TO aimee_kb_token_authority_store_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_authority_claim(TEXT,TEXT,TEXT,INTEGER) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_b64url_decode(TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_authority_finalize(TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_token_intent_kind(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_authority_readback(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_token_authority_snapshot(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_token_authority_admit(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_token_authority_use(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_token_authority_readback(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';
  EXECUTE 'ALTER FUNCTION public.kb_management_token_authority_finalize(TEXT,TEXT) OWNER TO aimee_kb_token_authority_definer';

  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_token_authority_definer;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_token_authority_definer;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_token_authority_definer;
  GRANT EXECUTE ON FUNCTION public.kb_management_token_authority_snapshot(TEXT,TEXT)
    TO aimee_kb_token_authority_definer;
  GRANT EXECUTE ON FUNCTION public.kb_management_read_b64url_decode(TEXT)
    TO aimee_kb_token_authority_definer;
  GRANT SELECT ON public.kb_management_token_intent_namespace,
    public.kb_management_action_intent,public.kb_management_read_intent,
    public.kb_management_read_key_use,
    public.kb_management_action_outcome,public.kb_team_membership,
    public.kb_admin_grant,public.kb_team_lead,public.kb_server_registry,
    public.kb_enrollments,public.kb_management_instance,
    public.kb_management_instance_issue,public.kb_cert_revocation_generation,
    public.kb_management_jwks_publication_registry,
    public.kb_management_jwks_publication_generation,
    public.kb_management_jwks_publication_candidate,
    public.kb_management_token_root,public.org_vault_current,
    public.org_vault_secret,public.org_vault_rotation,public.kb_vault_control,
    public.kb_management_token_key_use_intent TO aimee_kb_token_authority_definer;
  -- PostgreSQL requires UPDATE privilege on at least one column of each table
  -- named by SELECT ... FOR SHARE.  Grant only an identity/key column; the
  -- authority has no generic SQL seam and every actual mutation remains outside
  -- its fixed functions (the key-use table is additionally WORM-triggered).
  GRANT UPDATE(correlation_id) ON public.kb_management_token_intent_namespace,
    public.kb_management_action_intent,
    public.kb_management_action_outcome,public.kb_management_token_key_use_intent
    TO aimee_kb_token_authority_definer;
  GRANT UPDATE(state,lease_owner,lease_until,jwt,jwt_sha256,updated_at)
    ON public.kb_management_read_intent TO aimee_kb_token_authority_definer;
  GRANT UPDATE(id) ON public.kb_team_membership,public.kb_admin_grant,
    public.kb_team_lead,public.kb_enrollments,public.org_vault_secret,
    public.org_vault_rotation TO aimee_kb_token_authority_definer;
  GRANT UPDATE(server_id) ON public.kb_server_registry TO aimee_kb_token_authority_definer;
  GRANT UPDATE(installation_id) ON public.kb_management_instance
    TO aimee_kb_token_authority_definer;
  GRANT UPDATE(operation_id) ON public.kb_management_instance_issue
    TO aimee_kb_token_authority_definer;
  GRANT UPDATE(singleton) ON public.kb_cert_revocation_generation,
    public.kb_management_jwks_publication_registry,public.kb_vault_control
    TO aimee_kb_token_authority_definer;
  GRANT UPDATE(generation) ON public.kb_management_jwks_publication_generation,
    public.kb_management_jwks_publication_candidate TO aimee_kb_token_authority_definer;
  GRANT UPDATE(root_kind) ON public.kb_management_token_root
    TO aimee_kb_token_authority_definer;
  GRANT UPDATE(principal) ON public.org_vault_current TO aimee_kb_token_authority_definer;
  GRANT INSERT ON public.kb_management_token_key_use_intent
    TO aimee_kb_token_authority_definer;
  GRANT INSERT ON public.kb_management_read_key_use TO aimee_kb_token_authority_definer;
  GRANT EXECUTE ON FUNCTION
    public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
    TO aimee_kb_token_authority_definer;

  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_token_authority_runtime;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_token_authority_runtime;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_token_authority_runtime;
  GRANT EXECUTE ON FUNCTION
    public.kb_management_token_authority_admit(TEXT,TEXT),
    public.kb_management_token_authority_use(TEXT,TEXT),
    public.kb_management_token_authority_readback(TEXT,TEXT),
    public.kb_management_token_authority_finalize(TEXT,TEXT)
    TO aimee_kb_token_authority_runtime;
  -- The identity mint is reached by the same authority runtime role. Note the
  -- REVOKE ALL ON ALL FUNCTIONS above: without these grants the identity path
  -- fails with a permission error (42501) that classifies as DENIED, which is
  -- indistinguishable from a real authorization refusal at the C layer.
  GRANT EXECUTE ON FUNCTION
    public.kb_management_identity_authority_admit(TEXT,TEXT),
    public.kb_management_identity_authority_use(TEXT,TEXT),
    public.kb_management_identity_authority_readback(TEXT,TEXT),
    public.kb_management_identity_authority_finalize(TEXT,TEXT)
    TO aimee_kb_token_authority_runtime;
  GRANT EXECUTE ON FUNCTION
    public.kb_management_read_authority_claim(TEXT,TEXT,TEXT,INTEGER),
    public.kb_management_read_authority_finalize(TEXT,TEXT,TEXT,TEXT),
    public.kb_management_token_intent_kind(TEXT,TEXT),
    public.kb_management_read_authority_readback(TEXT,TEXT)
    TO aimee_kb_token_authority_runtime;

  REVOKE ALL ON TABLE public.kb_management_token_key_use_intent,
    public.kb_management_read_intent,public.kb_management_read_key_use FROM PUBLIC;
  REVOKE ALL ON FUNCTION public.kb_management_token_key_use_worm_guard(),
    public.kb_management_token_authority_snapshot(TEXT,TEXT),
    public.kb_management_token_authority_admit(TEXT,TEXT),
    public.kb_management_token_authority_use(TEXT,TEXT),
    public.kb_management_token_authority_readback(TEXT,TEXT),
    public.kb_management_token_authority_finalize(TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION public.kb_management_read_authority_claim(TEXT,TEXT,TEXT,INTEGER),
    public.kb_management_read_b64url_decode(TEXT),
    public.kb_management_read_authority_finalize(TEXT,TEXT,TEXT,TEXT),
    public.kb_management_token_intent_kind(TEXT,TEXT),
    public.kb_management_read_authority_readback(TEXT,TEXT) FROM PUBLIC;

  FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status',
    'aimee_kb_status_definer','aimee_kb_status_login','aimee_kb_status_authority',
    'aimee_kb_status_provision','aimee_kb_token_roots_provision','aimee_kb_jwks_publish',
    'aimee_kb_jwks_runtime_definer','aimee_kb_migrate'] LOOP
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
      EXECUTE format('REVOKE ALL ON TABLE public.kb_management_token_intent_namespace,'
        'public.kb_management_token_key_use_intent,public.kb_management_read_intent,'
        'public.kb_management_read_key_use FROM %I',role_name);
      EXECUTE format('REVOKE ALL ON FUNCTION '
        'public.kb_management_token_key_use_worm_guard(),'
        'public.kb_management_token_authority_snapshot(TEXT,TEXT),'
        'public.kb_management_token_authority_admit(TEXT,TEXT),'
        'public.kb_management_token_authority_use(TEXT,TEXT),'
        'public.kb_management_token_authority_readback(TEXT,TEXT),'
        'public.kb_management_token_authority_finalize(TEXT,TEXT) FROM %I',role_name);
      EXECUTE format('REVOKE ALL ON FUNCTION '
        'public.kb_management_read_authority_claim(TEXT,TEXT,TEXT,INTEGER),'
        'public.kb_management_read_b64url_decode(TEXT),'
        'public.kb_management_read_authority_finalize(TEXT,TEXT,TEXT,TEXT),'
        'public.kb_management_token_intent_kind(TEXT,TEXT),'
        'public.kb_management_read_authority_readback(TEXT,TEXT) FROM %I',role_name);
    END IF;
  END LOOP;
END
$$;

-- P5-C2a: only the dedicated offline provision role may invoke the fixed-root
-- state machine. It receives no direct table, sequence, generic vault, or audit
-- privilege; the owner-run functions carry the narrowly required authority.
DO $$
DECLARE role_name TEXT;
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_owner') THEN
    EXECUTE 'ALTER FUNCTION kb_management_token_root_registry_guard() OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_token_root_vault_guard() OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_token_root_slot(TEXT) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_token_root_bootstrap_resume(TEXT,TEXT) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_token_root_bootstrap_stage(TEXT,TEXT,TEXT,TEXT,BYTEA,BYTEA,BYTEA,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_token_root_bootstrap_record_cas(TEXT,TEXT,BYTEA) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_token_root_bootstrap_finalize(TEXT,TEXT) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_jwks_publication_root_inspect() OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION kb_management_jwks_publication_root_bind(TEXT,TEXT,TEXT,BYTEA,BYTEA) OWNER TO aimee_kb_owner';
    ALTER TABLE public.kb_management_token_root OWNER TO aimee_kb_owner;
    ALTER TABLE public.kb_management_jwks_publication_root OWNER TO aimee_kb_owner;
    ALTER TABLE public.kb_management_token_root_vault_permit OWNER TO aimee_kb_owner;
    GRANT SELECT,INSERT,UPDATE ON public.kb_management_token_root,
      public.kb_management_jwks_publication_root TO aimee_kb_owner;
    GRANT SELECT,INSERT,DELETE ON public.kb_management_token_root_vault_permit TO aimee_kb_owner;
  END IF;
  FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status',
      'aimee_kb_status_definer','aimee_kb_status_login','aimee_kb_migrate'] LOOP
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
      EXECUTE format('REVOKE ALL ON TABLE kb_management_token_root,kb_management_jwks_publication_root,kb_management_token_root_vault_permit FROM %I',role_name);
      EXECUTE format('REVOKE ALL ON FUNCTION '
        'kb_management_token_root_registry_guard(),'
        'kb_management_token_root_vault_guard(),'
        'kb_management_token_root_slot(TEXT),'
        'kb_management_token_root_bootstrap_resume(TEXT,TEXT),'
        'kb_management_token_root_bootstrap_stage(TEXT,TEXT,TEXT,TEXT,BYTEA,BYTEA,BYTEA,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA),'
        'kb_management_token_root_bootstrap_record_cas(TEXT,TEXT,BYTEA),'
        'kb_management_token_root_bootstrap_finalize(TEXT,TEXT),'
        'kb_management_jwks_publication_root_inspect(),'
        'kb_management_jwks_publication_root_bind(TEXT,TEXT,TEXT,BYTEA,BYTEA) FROM %I',role_name);
    END IF;
  END LOOP;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_token_roots_provision') THEN
    REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_token_roots_provision;
    REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_token_roots_provision;
    REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_token_roots_provision;
    GRANT EXECUTE ON FUNCTION
      kb_management_token_root_bootstrap_resume(TEXT,TEXT),
      kb_management_token_root_bootstrap_stage(TEXT,TEXT,TEXT,TEXT,BYTEA,BYTEA,BYTEA,BIGINT,BYTEA,
        BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA),
      kb_management_token_root_bootstrap_record_cas(TEXT,TEXT,BYTEA),
      kb_management_token_root_bootstrap_finalize(TEXT,TEXT),
      kb_management_jwks_publication_root_inspect(),
      kb_management_jwks_publication_root_bind(TEXT,TEXT,TEXT,BYTEA,BYTEA)
      TO aimee_kb_token_roots_provision;
  END IF;
END
$$;

-- P5-C2b immutable generation-1 JWKS publisher.  The offline execution role
-- receives only the fixed state-machine facade and no raw table/vault access.
DO $$
DECLARE role_name TEXT;
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_owner') THEN
    ALTER TABLE public.kb_management_jwks_publication_candidate OWNER TO aimee_kb_owner;
    ALTER TABLE public.kb_management_jwks_publication_generation OWNER TO aimee_kb_owner;
    ALTER TABLE public.kb_management_jwks_publication_registry OWNER TO aimee_kb_owner;
    ALTER TABLE public.kb_management_jwks_manifest_key_use_intent OWNER TO aimee_kb_owner;
    ALTER TABLE public.kb_management_jwks_publication_permit OWNER TO aimee_kb_owner;
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_guard() OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_inspect() OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_roots() OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_manifest_key_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,BYTEA,BYTEA) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_stage(BIGINT,TEXT,BIGINT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,TEXT,BYTEA,BYTEA,TEXT,BYTEA,BYTEA,BYTEA,BIGINT) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_record_cas(BIGINT,TEXT,BYTEA) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_finalize(BIGINT,TEXT) OWNER TO aimee_kb_owner';
    EXECUTE 'ALTER FUNCTION public.kb_management_jwks_publication_final() OWNER TO aimee_kb_owner';
    GRANT SELECT ON public.kb_management_token_root,
      public.kb_management_jwks_publication_root,public.org_vault_current,
      public.org_vault_secret,public.kb_vault_control TO aimee_kb_owner;
    GRANT SELECT,INSERT,UPDATE,DELETE ON public.kb_management_jwks_publication_candidate,
      public.kb_management_jwks_publication_generation,
      public.kb_management_jwks_publication_registry,
      public.kb_management_jwks_manifest_key_use_intent,
      public.kb_management_jwks_publication_permit TO aimee_kb_owner;
    GRANT EXECUTE ON FUNCTION public.org_vault_control_require_open(),
      public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_owner;
  END IF;
  FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status',
    'aimee_kb_status_definer','aimee_kb_status_login','aimee_kb_status_authority',
    'aimee_kb_status_provision','aimee_kb_token_roots_provision',
    'aimee_kb_migrate','aimee_kb_jwks_publish','aimee_kb_jwks_runtime_definer'] LOOP
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
      EXECUTE format('REVOKE ALL ON TABLE '
       'kb_management_jwks_publication_candidate,kb_management_jwks_publication_generation,'
       'kb_management_jwks_publication_registry,kb_management_jwks_manifest_key_use_intent,'
       'kb_management_jwks_publication_permit FROM %I',role_name);
      EXECUTE format('REVOKE ALL ON FUNCTION '
       'kb_management_jwks_publication_guard(),kb_management_jwks_publication_inspect(),'
       'kb_management_jwks_publication_roots(),'
       'kb_management_jwks_manifest_key_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,BYTEA,BYTEA),'
       'kb_management_jwks_publication_stage(BIGINT,TEXT,BIGINT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,TEXT,BYTEA,BYTEA,TEXT,BYTEA,BYTEA,BYTEA,BIGINT),'
       'kb_management_jwks_publication_record_cas(BIGINT,TEXT,BYTEA),'
       'kb_management_jwks_publication_finalize(BIGINT,TEXT),'
       'kb_management_jwks_publication_final(),'
       'kb_management_jwks_runtime_fetch(TEXT,TEXT,TEXT) FROM %I',role_name);
    END IF;
  END LOOP;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_jwks_publish') THEN
    REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_jwks_publish;
    REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_jwks_publish;
    REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_jwks_publish;
    GRANT EXECUTE ON FUNCTION public.kb_management_jwks_publication_inspect(),
      public.kb_management_jwks_publication_roots(),
      public.kb_management_jwks_manifest_key_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,BYTEA,BYTEA),
      public.kb_management_jwks_publication_stage(BIGINT,TEXT,BIGINT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,TEXT,BYTEA,BYTEA,TEXT,BYTEA,BYTEA,BYTEA,BIGINT),
      public.kb_management_jwks_publication_record_cas(BIGINT,TEXT,BYTEA),
      public.kb_management_jwks_publication_finalize(BIGINT,TEXT),
      public.kb_management_jwks_publication_final() TO aimee_kb_jwks_publish;
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN
    GRANT EXECUTE ON FUNCTION public.kb_management_jwks_runtime_fetch(TEXT,TEXT,TEXT)
      TO aimee_kb_runtime;
  END IF;
END
$$;

-- P5-C2c online JWKS reader.  A dedicated non-login definer is the only role
-- allowed to cross FORCE RLS for the certificate tuple.  It has read-only
-- access to the exact join closure and invokes (but does not own) the existing
-- vault barrier helper.  Runtime can execute only the fixed facade and is not a
-- member of this role.
DO $$
DECLARE role_name TEXT;
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_jwks_runtime_definer') THEN
    RAISE EXCEPTION 'aimee_kb_jwks_runtime_definer role is required';
  END IF;
  EXECUTE 'ALTER FUNCTION public.kb_management_jwks_runtime_fetch(TEXT,TEXT,TEXT) OWNER TO aimee_kb_jwks_runtime_definer';
  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_jwks_runtime_definer;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_jwks_runtime_definer;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_jwks_runtime_definer;
  GRANT SELECT ON public.kb_server_registry,public.kb_enrollments,
    public.kb_management_jwks_publication_candidate,
    public.kb_management_jwks_publication_generation,
    public.kb_management_jwks_publication_registry TO aimee_kb_jwks_runtime_definer;
  GRANT EXECUTE ON FUNCTION public.org_vault_control_require_open()
    TO aimee_kb_jwks_runtime_definer;
  FOREACH role_name IN ARRAY ARRAY['aimee_kb_owner','aimee_kb_status',
    'aimee_kb_status_definer','aimee_kb_status_login','aimee_kb_status_authority',
    'aimee_kb_status_provision','aimee_kb_token_roots_provision',
    'aimee_kb_migrate','aimee_kb_jwks_publish'] LOOP
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
      EXECUTE format('REVOKE ALL ON FUNCTION '
        'public.kb_management_jwks_runtime_fetch(TEXT,TEXT,TEXT) FROM %I',role_name);
    END IF;
  END LOOP;
END
$$;

-- P5-C1c immutable management-action journal.  The runtime role has no table or
-- guard access and crosses FORCE RLS only through the two fixed owner-definer
-- functions.  The raw audit appender remains unavailable to runtime.
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_owner') THEN
    RETURN;
  END IF;

  ALTER TABLE public.kb_management_action_intent OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_management_action_outcome OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_management_token_intent_namespace OWNER TO aimee_kb_owner;
  EXECUTE 'ALTER FUNCTION public.kb_management_action_worm_guard() OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_action_intent_start(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_identity_intent_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_identity_login_context(BIGINT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_action_outcome_append(TEXT,TEXT,TEXT,INTEGER,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_publication_generation() OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_intent_start(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA,TEXT,TEXT,INTEGER,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_read_token_readback(TEXT,TEXT) OWNER TO aimee_kb_owner';

  GRANT SELECT,UPDATE ON public.kb_server_registry TO aimee_kb_owner;
  GRANT SELECT ON public.kb_management_instance,
    public.kb_management_instance_issue,public.kb_enrollments,
    public.kb_cert_revocation_generation,public.kb_admin_grant,
    public.kb_team_lead,public.kb_team_membership,public.kb_audit_event TO aimee_kb_owner;
  GRANT SELECT,INSERT ON public.kb_management_read_intent TO aimee_kb_owner;
  -- The identity intent writer runs as this owner; the mint pipeline still runs
  -- as the token-authority definer, so INSERT/SELECT is all the writer needs.
  GRANT SELECT,INSERT ON public.kb_management_identity_intent TO aimee_kb_owner;
  GRANT SELECT ON public.kb_write_tier_grant TO aimee_kb_owner;
  GRANT SELECT ON public.kb_management_jwks_publication_registry,
    public.kb_management_jwks_publication_generation,
    public.kb_management_jwks_publication_candidate,public.kb_management_token_root
    TO aimee_kb_owner;
  GRANT EXECUTE ON FUNCTION public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
    TO aimee_kb_owner;

  REVOKE ALL ON TABLE public.kb_management_token_intent_namespace,
    public.kb_management_action_intent,
    public.kb_management_action_outcome,public.kb_management_read_intent,
    public.kb_management_read_key_use FROM PUBLIC;
  REVOKE ALL ON FUNCTION public.kb_management_action_worm_guard(),
    public.kb_management_action_intent_start(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT),
    public.kb_management_identity_intent_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT),
    public.kb_management_identity_login_context(BIGINT),
    public.kb_management_action_outcome_append(TEXT,TEXT,TEXT,INTEGER,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION public.kb_management_read_transition_guard(),
    public.kb_management_read_worm_guard(),
    public.kb_management_read_publication_generation(),
    public.kb_management_read_intent_start(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA,TEXT,TEXT,INTEGER,TEXT),
    public.kb_management_read_token_readback(TEXT,TEXT) FROM PUBLIC;

  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN
    REVOKE ALL ON TABLE public.kb_management_token_intent_namespace,
      public.kb_management_action_intent,
      public.kb_management_action_outcome,public.kb_management_read_intent,
      public.kb_management_read_key_use FROM aimee_kb_runtime;
    REVOKE ALL ON FUNCTION public.kb_management_action_worm_guard(),
      public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
      FROM aimee_kb_runtime;
    GRANT EXECUTE ON FUNCTION
      public.kb_management_action_intent_start(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT),
      public.kb_management_identity_intent_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT),
      public.kb_management_identity_login_context(BIGINT),
      public.kb_management_action_outcome_append(TEXT,TEXT,TEXT,INTEGER,TEXT)
      TO aimee_kb_runtime;
    GRANT EXECUTE ON FUNCTION
      public.kb_management_read_publication_generation(),
      public.kb_management_read_intent_start(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA,TEXT,TEXT,INTEGER,TEXT)
      TO aimee_kb_runtime;
  END IF;
END
$$;

-- P5-B2b management-instance lineage.  The broad compatibility grants near the
-- top of this file must never expose the primary-only lineage tables.  Runtime
-- crosses FORCE RLS only through the six fixed owner-definer entry points;
-- offline grant and replacement provisioning remains migration-only.
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_owner') THEN
    RETURN;
  END IF;

  ALTER TABLE public.kb_management_instance_grant OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_management_instance OWNER TO aimee_kb_owner;
  ALTER TABLE public.kb_management_instance_issue OWNER TO aimee_kb_owner;

  EXECUTE 'ALTER FUNCTION public.kb_management_instance_grant_guard() OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_guard() OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_issue_guard() OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_binding_digest(TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_grant_create(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_replacement_grant_create(TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_grant_preflight(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_begin_initial(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_begin_renewal(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_activate(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_snapshot(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION public.kb_management_instance_expire_quarantine(INTEGER) OWNER TO aimee_kb_owner';

  -- The definer needs only the existing rows touched by activation/replacement.
  GRANT SELECT ON public.kb_admin_grant TO aimee_kb_owner;
  GRANT SELECT,INSERT,UPDATE ON public.kb_enrollments TO aimee_kb_owner;
  GRANT SELECT,INSERT ON public.kb_team_membership TO aimee_kb_owner;
  GRANT SELECT,UPDATE ON public.kb_cert_revocation_generation TO aimee_kb_owner;
  GRANT SELECT ON public.kb_audit_event TO aimee_kb_owner;
  GRANT USAGE,SELECT ON SEQUENCE public.kb_enrollments_id_seq,
    public.kb_team_membership_id_seq TO aimee_kb_owner;
  GRANT EXECUTE ON FUNCTION public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
    TO aimee_kb_owner;

  REVOKE ALL ON TABLE public.kb_management_instance_grant,
    public.kb_management_instance,public.kb_management_instance_issue FROM PUBLIC;
  REVOKE ALL ON FUNCTION
    public.kb_management_instance_grant_guard(),
    public.kb_management_instance_guard(),
    public.kb_management_instance_issue_guard(),
    public.kb_management_instance_binding_digest(TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_grant_create(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_replacement_grant_create(TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_grant_preflight(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_begin_initial(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_begin_renewal(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_activate(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT),
    public.kb_management_instance_snapshot(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
    public.kb_management_instance_expire_quarantine(INTEGER) FROM PUBLIC;

  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN
    REVOKE ALL ON TABLE public.kb_management_instance_grant,
      public.kb_management_instance,public.kb_management_instance_issue
      FROM aimee_kb_runtime;
    REVOKE ALL ON FUNCTION
      public.kb_management_instance_grant_guard(),
      public.kb_management_instance_guard(),
      public.kb_management_instance_issue_guard(),
      public.kb_management_instance_binding_digest(TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_grant_create(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_replacement_grant_create(TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
      FROM aimee_kb_runtime;
    GRANT EXECUTE ON FUNCTION
      public.kb_management_instance_grant_preflight(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_begin_initial(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_begin_renewal(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_activate(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT),
      public.kb_management_instance_snapshot(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_expire_quarantine(INTEGER) TO aimee_kb_runtime;
  END IF;

  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_migrate') THEN
    GRANT EXECUTE ON FUNCTION
      public.kb_management_instance_grant_create(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT),
      public.kb_management_instance_replacement_grant_create(TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT)
      TO aimee_kb_migrate;
  END IF;
END
$$;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime') THEN
    RETURN;
  END IF;
  -- Reassert after the broad runtime grants above: the online status key is
  -- reachable only through the distinct status role's fixed definer API.
  -- Provisioning is roles -> schema -> grants; a runtime role created outside
  -- that contract receives no default privileges, and the service boot/PG gates
  -- reject any deployment whose final ACLs differ from this block.
  REVOKE ALL ON TABLE kb_management_status_key,kb_management_status_key_use_intent
    FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION kb_management_status_key_candidate(TEXT,TEXT,BIGINT),
    kb_management_status_key_admit(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BYTEA),
    kb_management_status_key_use_guard(BIGINT),kb_management_status_key_startup_status(),
    kb_management_status_key_bootstrap_stage(TEXT,TEXT,TEXT,BYTEA,BYTEA,
      BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA),
    kb_management_status_key_bootstrap_resume(TEXT,TEXT),
    kb_management_status_key_bootstrap_prepare_activation(TEXT),
    kb_management_status_key_bootstrap_finalize(TEXT,BYTEA)
    FROM aimee_kb_runtime;
END
$$;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_owner') THEN
    RETURN;
  END IF;
  EXECUTE 'ALTER FUNCTION kb_management_status_key_bootstrap_stage(TEXT,TEXT,TEXT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION kb_management_status_key_bootstrap_resume(TEXT,TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION kb_management_status_key_bootstrap_prepare_activation(TEXT) OWNER TO aimee_kb_owner';
  EXECUTE 'ALTER FUNCTION kb_management_status_key_bootstrap_finalize(TEXT,BYTEA) OWNER TO aimee_kb_owner';
  ALTER TABLE public.kb_management_status_key OWNER TO aimee_kb_owner;
  ALTER TABLE public.org_vault_secret OWNER TO aimee_kb_owner;
  ALTER TABLE public.org_vault_current OWNER TO aimee_kb_owner;
  ALTER TABLE public.org_vault_rotation OWNER TO aimee_kb_owner;
  GRANT EXECUTE ON FUNCTION public.org_vault_control_require_open(),
    public.kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_owner;
  GRANT SELECT,INSERT,UPDATE ON public.kb_management_status_key,
    public.org_vault_secret,public.org_vault_current,public.org_vault_rotation
    TO aimee_kb_owner;
  GRANT SELECT ON public.kb_audit_event TO aimee_kb_owner;
  GRANT USAGE,SELECT ON SEQUENCE public.org_vault_secret_id_seq,
    public.org_vault_rotation_id_seq TO aimee_kb_owner;
  REVOKE ALL ON FUNCTION
    kb_management_status_key_bootstrap_stage(TEXT,TEXT,TEXT,BYTEA,BYTEA,
      BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA),
    kb_management_status_key_bootstrap_resume(TEXT,TEXT),
    kb_management_status_key_bootstrap_prepare_activation(TEXT),
    kb_management_status_key_bootstrap_finalize(TEXT,BYTEA) FROM PUBLIC;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_migrate') THEN
    GRANT EXECUTE ON FUNCTION
      kb_management_status_key_bootstrap_stage(TEXT,TEXT,TEXT,BYTEA,BYTEA,
        BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA),
      kb_management_status_key_bootstrap_resume(TEXT,TEXT),
      kb_management_status_key_bootstrap_prepare_activation(TEXT),
      kb_management_status_key_bootstrap_finalize(TEXT,BYTEA) TO aimee_kb_migrate;
  END IF;
END
$$;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_status_definer') THEN
    RETURN;
  END IF;
  -- Provision the narrow fixed definer after schema.sql. Keeping ownership here
  -- also permits a schema-only developer load when roles are not installed.
  EXECUTE 'ALTER FUNCTION kb_management_status_key_candidate(TEXT,TEXT,BIGINT) OWNER TO aimee_kb_status_definer';
  EXECUTE 'ALTER FUNCTION kb_management_status_key_admit(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BYTEA) OWNER TO aimee_kb_status_definer';
  EXECUTE 'ALTER FUNCTION kb_management_status_key_use_guard(BIGINT) OWNER TO aimee_kb_status_definer';
  EXECUTE 'ALTER FUNCTION kb_management_status_key_startup_status() OWNER TO aimee_kb_status_definer';
  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_status_definer;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_status_definer;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_status_definer;
  GRANT SELECT ON kb_management_status_key,org_vault_current,org_vault_secret,
    org_vault_rotation,kb_enrollments,kb_server_registry,kb_team_membership,
    kb_cert_revocation_generation TO aimee_kb_status_definer;
  GRANT UPDATE ON kb_management_status_key,org_vault_secret,kb_enrollments,
    kb_server_registry,kb_team_membership,kb_cert_revocation_generation
    TO aimee_kb_status_definer;
  GRANT SELECT,INSERT,UPDATE ON kb_management_status_key_use_intent
    TO aimee_kb_status_definer;
  GRANT EXECUTE ON FUNCTION org_vault_control_require_open(),
    org_vault_control_startup_status(),
    kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_status_definer;
END
$$;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_status') THEN
    RETURN;
  END IF;
  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_status;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_status;
  REVOKE ALL ON TABLE kb_management_status_key,kb_management_status_key_use_intent
    FROM aimee_kb_status;
  REVOKE ALL ON FUNCTION kb_management_status_lookup(TEXT,TEXT,TEXT,TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION kb_management_status_lookup(TEXT,TEXT,TEXT,TEXT,TEXT)
    TO aimee_kb_status;
  REVOKE ALL ON FUNCTION kb_management_action_checkpoint(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT)
    FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION kb_management_action_checkpoint(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT)
    TO aimee_kb_status;
  GRANT EXECUTE ON FUNCTION kb_management_status_key_candidate(TEXT,TEXT,BIGINT),
    kb_management_status_key_admit(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BYTEA),
    kb_management_status_key_use_guard(BIGINT),
    kb_management_status_key_startup_status() TO aimee_kb_status;
END
$$;

DO $$
DECLARE role_name TEXT;
BEGIN
  FOREACH role_name IN ARRAY ARRAY[
    'aimee_kb_runtime','aimee_kb_status','aimee_kb_status_definer','aimee_kb_status_login'
  ] LOOP
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
      EXECUTE format('REVOKE ALL ON FUNCTION '
        'kb_management_status_key_bootstrap_stage(TEXT,TEXT,TEXT,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA,BYTEA),'
        'kb_management_status_key_bootstrap_resume(TEXT,TEXT),'
        'kb_management_status_key_bootstrap_prepare_activation(TEXT),'
        'kb_management_status_key_bootstrap_finalize(TEXT,BYTEA) FROM %I',role_name);
    END IF;
  END LOOP;
END
$$;
