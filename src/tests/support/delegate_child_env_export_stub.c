/* Stub of delegate_child_export_context_env for tests that link the fork-site
 * objects (provider_cli_adapter.o, delegate_backend_local.o) but not
 * server_compute.o (where the real implementation lives). The exporter only runs
 * in a post-fork child, which these unit tests do not exercise. */
void delegate_child_export_context_env(void)
{
}
