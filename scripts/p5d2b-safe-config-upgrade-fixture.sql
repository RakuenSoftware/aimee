ALTER TABLE kb_management_read_intent
  DROP CONSTRAINT kb_management_read_intent_selector_check,
  DROP CONSTRAINT kb_management_read_intent_external_path_check,
  DROP CONSTRAINT kb_management_read_intent_path_binding_check;
ALTER TABLE kb_management_read_key_use
  DROP CONSTRAINT kb_management_read_key_use_selector_check;
ALTER TABLE kb_management_read_intent
  ADD CONSTRAINT legacy_agents_only_alpha CHECK (selector='agents'),
  ADD CONSTRAINT legacy_agents_path_beta
    CHECK (external_path ~ '^/v1/servers/[A-Za-z0-9][A-Za-z0-9._-]{0,126}/agents$'),
  ADD CONSTRAINT legacy_agents_binding_gamma
    CHECK (external_path='/v1/servers/'||target_server_id||'/agents'),
  ADD CONSTRAINT preserve_selector_capability
    CHECK (selector<>'config' OR capability='remote_reads'),
  ADD CONSTRAINT preserve_path_method
    CHECK (external_path NOT LIKE '%..%' AND external_method='GET');
ALTER TABLE kb_management_read_key_use
  ADD CONSTRAINT legacy_agents_key_use_delta CHECK (selector='agents'),
  ADD CONSTRAINT preserve_key_use_selector_team CHECK (selector<>'config' OR team_id>0);
