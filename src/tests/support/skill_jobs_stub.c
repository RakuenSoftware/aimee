/* skill_jobs_stub.c: test stubs for skill review / curator async dispatch.
 * Used by unit-test-server-dispatch to avoid linking the full compute pool. */

/* Forward-declare only what we need to satisfy the linker. */
typedef struct server_ctx server_ctx_t;

void server_compute_skill_review_async(server_ctx_t *ctx, const char *session_id)
{
   (void)ctx;
   (void)session_id;
}

/* The dispatch test isolates server.c from the event-bus runtime. The module
 * handler's modulo semantics are covered by unit-test-process-module-handlers. */
int server_module_skill_should_fire(int hook_count, int interval, int *fire)
{
   if (!fire)
      return -1;
   *fire = interval > 0 && hook_count > 0 && (hook_count % interval) == 0;
   return 0;
}
