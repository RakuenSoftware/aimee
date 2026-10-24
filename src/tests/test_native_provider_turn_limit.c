/* Exercise the actual adapter configuration path, not a duplicate cap reducer.
 * Only the host/OS boundary is substituted; no SDK/provider can be started. */
#include "../posix/agent_runtime.c"
#include <assert.h>

extern int native_limit_test_tls;
extern int native_limit_test_observed;
extern int native_limit_test_calls;
extern int native_limit_test_host_unavailable;

int main(void)
{
   static const struct
   {
      int tls, configured, expected;
   } cases[] = {{0, 3, 3}, {2, 3, 2},  {7, 3, 3},  {4, 0, 4},
                {0, 0, 0}, {-1, 5, 5}, {0, -1, 0}, {-1, -1, 0}};
   const provider_cli_adapter_t adapter = {.display_name = "fixture"};
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
   {
      agent_t selected = {.max_turns = cases[i].configured};
      native_limit_test_tls = cases[i].tls;
      native_limit_test_observed = -999;
      native_limit_test_calls = 0;
      agent_result_t result = {0};
      assert(agent_execute_provider_session(&adapter, &selected, NULL, NULL, "system", "task", NULL,
                                            NULL, &result) == AGENT_RC_CONTEXT_REFUSED);
      assert(native_limit_test_calls == 1);
      assert(native_limit_test_observed == cases[i].expected);
      assert(!result.success && !result.canonical_response.n_content);
      agent_result_clear_response(&result);
   }
   /* Even an unseeded request without any capture or credential ownership must
    * refuse when the canonical host owner is absent. Boundary stubs abort if
    * credential inspection or the old standalone executor is reached. */
   agent_t selected = {.max_turns = 3};
   agent_result_t unavailable = {0};
   native_limit_test_host_unavailable = 1;
   assert(agent_execute_provider_session(&adapter, &selected, NULL, NULL, "system", "task", NULL,
                                         NULL, &unavailable) == AGENT_RC_CONTEXT_REFUSED);
   assert(!unavailable.success && !unavailable.canonical_response.n_content);
   assert(strstr(unavailable.error, "execution owner unavailable"));
   puts("native adapter preserves configured/TLS minimum turn caps: ok");
   return 0;
}
