/* cli_marshal_defs_data.h: methods whose request body is empty.
 *
 * A method taking no arguments needs no marshalling code -- the body is {} --
 * so this is the one part of argument handling that is pure data, and the part
 * whose absence still blocked the goal: with routes, catalogue and dispatch
 * served, a NEW no-arg command was discoverable and addressable but still
 * refused with "arguments are missing or invalid", because the client had no
 * row saying it needed none.
 *
 * The methods that map argv to fields need a spec carrying types before they
 * can be served this way. The ones that read the client's own disk or
 * environment (marshal_git_cli, marshal_skill_request, marshal_index_file_request,
 * ...) should NOT be: reading local state to send it is the thin client's own
 * job, not knowledge of what the server can do.
 *
 * Included inside an array initializer; the array lives with each includer.
 */
    "api.disable",
    "api.status",
    "audit.checkpoint",
    "audit.seal",
    "audit.snapshot",
    "audit.verify",
    "aux.config_show",
    "calibration.readiness",
    "cert.list",
    "config.deploy_env",
    "config.show",
    "cron.list",
    "delegate.backend_list",
    "delegate.sandbox_list",
    "demotion.check",
    "doctor.forensics",
    "economizer.stats",
    "episode.list",
    "hud.status",
    "identity.show",
    "kb.curator",
    "kb.health",
    "kb.ingest.status",
    "mcp.audit",
    "memory.stats",
    "catalog.refresh",
    "notes.list",
    "provider.get",
    "ranker.export_view",
    "ranker.fit",
    "rules.generate",
    "rules.list",
    "server.health",
    "toolset.list",
    "vault.list",
    "vault.lock",
    "workers",
    "workspace.list",
