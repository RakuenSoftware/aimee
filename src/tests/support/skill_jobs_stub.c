/* skill_jobs_stub.c: test stubs for skill review / curator async dispatch.
 * Used by unit-test-server-dispatch to avoid linking the full compute pool. */

/* Forward-declare only what we need to satisfy the linker. */
typedef struct server_ctx server_ctx_t;

void server_compute_skill_review_async(server_ctx_t *ctx, const char *session_id)
{
   (void)ctx;
   (void)session_id;
}
