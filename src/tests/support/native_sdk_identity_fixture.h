/* Exercise owned profile identity before cache scope and model replacement. */
static void test_native_sdk_identity(const agent_t *selected, const request_context_t *owner)
{
   owner_tools(1);
   aimee_block_t system = {.type = AIMEE_BLK_TEXT,
                           .text = "host-owned system",
                           .cache_control = "ephemeral",
                           .cache_ttl = "1h",
                           .message_group = 7};
   aimee_tool_t function = {.name = "read_file"};
   aimee_tool_t tools = {
       .kind = AIMEE_TOOL_NAMESPACE, .name = "aimee", .children = &function, .n_children = 1};
   aimee_request_t seed = {.model = "selected-model",
                           .frontend = AIMEE_WIRE_ANTHROPIC,
                           .system = &system,
                           .n_system = 1,
                           .tools = &tools,
                           .n_tools = 1,
                           .prompt_cache_key = "",
                           .mutated = 1};
   aimee_request_t profile = {0};
   assert(!aimee_ir_request_clone(&seed, &profile));
   assert(server_native_sdk_profile_prepare(&profile, "claude", -1) < 0);
   assert(profile.n_system == 1);
   profile.mutated = 0;
   assert(!server_native_sdk_profile_prepare(&profile, "codex", 3));
   assert(!profile.mutated);
   assert(profile.n_system == 1); /* Other SDK profiles never acquire Claude identity. */
   assert(server_native_sdk_profile_prepare(&profile, "unknown", 3) < 0);
   profile.mutated = 1;
   server_session_capture_t *capture =
       server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
   assert(capture && !server_session_capture_bind(capture));
   server_native_runtime_t *baseline = server_native_runtime_open_bound_request(
       selected, owner, "session-a", &profile, &read_binding, 1, NULL);
   assert(baseline);
   server_native_execution_profile_t execution = {
       .sdk = "claude",
       .build_digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       .launch_digest = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       .workspace_digest = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"};
   char before[65], after[65], repeated[65];
   assert(!server_native_runtime_cache_scope(baseline, &execution, before));
   profile.mutated = 0;
   assert(!profile.raw && !profile.raw_bytes && !profile.raw_length);
   assert(!server_native_sdk_profile_prepare(&profile, "claude", 3));
   assert(profile.mutated);
   assert(profile.n_system == 2 && !strcmp(profile.system[0].text, AIMEE_NATIVE_CLAUDE_IDENTITY));
   assert(!strcmp(profile.system[1].text, "host-owned system"));
   assert(!profile.system[0].cache_control && !profile.system[0].cache_ttl);
   assert(!strcmp(profile.system[1].cache_control, "ephemeral") &&
          !strcmp(profile.system[1].cache_ttl, "1h") && profile.system[1].message_group == 7);
   assert(profile.system[0].context.origin == AIMEE_CTX_ORIGIN_PLATFORM &&
          profile.system[0].context.authority == AIMEE_CTX_AUTH_POLICY &&
          profile.system[0].context.trust == AIMEE_CTX_TRUST_VERIFIED &&
          profile.system[0].context.model_visible);
   server_native_runtime_t *runtime = server_native_runtime_open_bound_request(
       selected, owner, "session-a", &profile, &read_binding, 1, NULL);
   server_native_runtime_t *same = server_native_runtime_open_bound_request(
       selected, owner, "session-a", &profile, &read_binding, 1, NULL);
   assert(runtime && same && !server_native_runtime_attest_launch(runtime, transport_launch));
   assert(!server_native_runtime_cache_scope(runtime, &execution, after));
   assert(!server_native_runtime_cache_scope(same, &execution, repeated));
   assert(strcmp(before, after) && !strcmp(after, repeated));
   aimee_request_free(&profile);
   assert(server_session_capture_bind(NULL) == capture);
   atomic_store(&expected_capture, capture);
   captured_rows = cJSON_CreateArray();
   atomic_store(&owned_profile_probe, 1);
   atomic_store(&sdk_identity_probe, 1);
   atomic_store(&snapshot_generation, 1);
   char response[1024];
   request(runtime, "/v1/messages", server_native_runtime_token(runtime), response);
   assert(strstr(response, "200 OK") && !server_session_capture_failed(capture));
   server_native_runtime_close(same);
   server_native_runtime_close(runtime);
   server_native_runtime_close(baseline);
   atomic_store(&sdk_identity_probe, 0);
   atomic_store(&owned_profile_probe, 0);
   atomic_store(&snapshot_generation, 0);
   server_session_capture_close(capture);
   atomic_store(&expected_capture, NULL);
   cJSON_Delete(captured_rows);
   captured_rows = NULL;
}
