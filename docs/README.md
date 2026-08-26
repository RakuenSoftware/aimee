# Documentation

Start here:

| If you want to… | Read |
| --- | --- |
| install aimee | [Quickstart](QUICKSTART.md) |
| deploy or upgrade services | [Deployment](DEPLOYMENT.md) and [upgrading](UPGRADING.md) |
| use the CLI and browser | [Manual](../MANUAL.md) |
| understand the system | [Architecture](ARCHITECTURE.md) |
| understand the new runtime spine | [Event bus](EVENT_BUS.md) |
| configure a deployment | [Settings](SETTINGS.md) and [generated configuration](gen/configuration.md) |
| call the API | [Public API](PUBLIC_API.md) and [generated routes](gen/api-v1.md) |
| find a command | [Generated command reference](gen/cli-commands.md) |
| operate delegates and workflows | [Delegates](DELEGATES.md), [sandbox](DELEGATE_SANDBOX.md), [role permissions](DELEGATE_ROLE_PERMISSIONS.md), and [workflows](WORKFLOWS.md) |
| understand memory and retrieval | [Knowledge](KNOWLEDGE.md), [curator](CURATOR_PIPELINE.md), and [retrieval](retrieval-stack.md) |
| understand KB scaling and model placement | [KB fleet and model placement](KB_FLEET.md) |
| check support or feature state | [Compatibility](COMPATIBILITY.md) and [status](STATUS.md) |
| diagnose a failure | [Troubleshooting](TROUBLESHOOTING.md) |
| upgrade from the last release | [What's new](WHATS_NEW.md) and [upgrading](UPGRADING.md) |
| cut a release | [Releasing](RELEASING.md) |
| write or review documentation | [Documentation voice and maintenance](WRITING.md) |
| set budgets, rate limits, or a model catalog | [Teams, budgets, and rate limits](ORG_GOVERNANCE.md) |
| work on aimee itself | [Technical reference](../src/README.md) and [owners](../OWNERS.md) |
| see where the project is going | [Roadmap](ROADMAP.md) and [proposals](PROPOSALS.md) |

## Product guides

- [Commands](COMMANDS.md)
- [Code intelligence](CODE_INTELLIGENCE.md)
- [Roundtables](ENSEMBLE.md)
- [Workflows](WORKFLOWS.md) and [workflow actions](WORKFLOW_ACTIONS.md)
- [Autonomous development](AUTONOMOUS_DEVELOPMENT.md)
- [Workspaces](WORKSPACES.md)
- [Personas](personas.md)
- [Anchored editing](anchored-editing.md)
- [Structured PDF ingestion](STRUCTURED_PDF.md)
- [Choosing a synthesis model](SYNTHESIS_MODELS.md): which model to run, measured
- [KB inference](KB_LLM_BACKENDS.md), [local inference](LOCAL_INFERENCE.md) and [synthesis tiers](AIMEE_KB_SYNTH_TIERS.md)
- [Embedder selection](embedder-sweep.md)
- [CSS render sidecar](../deploy/css-render/README.md)
- [Browser workspace](DASHBOARD.md), [VS Code](VSCODE.md), and [KB console](KB_CONSOLE.md)
- [Context economizer](features/economizer.md), [tool-output condensation](features/tool-output-condensation.md),
  and [canonical response parsing](features/ir-only-response-parsing.md)
- [Agent reference](agent.md), returned by the `get_help` MCP tool

## Security and operations

- [Security model](SECURITY.md)
- [Storage ownership](STORAGE_TIERS.md)
- [Thin clients](THIN_CLIENT.md)
- [Web git security](WEBCHAT_GIT_SECURITY.md)
- [Sandbox verification](DELEGATE_SANDBOX_VERIFY.md)
- [Benchmarks](BENCHMARKS.md)
- [Change the KB embedder](runbooks/change-embedder.md)
- [Vault key rotation](runbooks/vault-master-key-rotation.md)
- [Witness evidence and egress gate](runbooks/witness-evidence-and-egress-gate.md)
- [Workflow autonomy](wfe-autonomy-runbook.md)
- [Appliance state recovery](runbooks/appliance-state-recovery.md)
- [Retrieval readiness and recovery](runbooks/retrieval-readiness-and-recovery.md)
- [Virtual-context alerts](observability/virtual-context-alerts.md)
- [WORM audit worker](WORM_WORKER.md)

## Engineering

- [Module contracts](modules/README.md), including [db3 routing and admission](db3.md)
- [Technical reference](../src/README.md)
- [Core C libraries](core/connection.md): [event bus](core/event-bus.md) and
  [repository extraction](core/repository-extraction.md)
- [Turn integrity](architecture/turn-integrity.md)
- [Event bus decisions](dev/EVENT_BUS_DECISIONS.md), [feature tree](dev/EVENT_BUS_FEATURE_TREE.md),
  and [arena payloads](dev/EVENT_BUS_ARENA_GUIDE.md)
- [Fold pipeline order](dev/fold-pipeline-order.md)
- [Go rewrite direction](dev/GO_REWRITE.md)
- [Workflow ownership](dev/WFE_OWNERSHIP.md)
- [Sanitizer call-site register](SANITIZER_CALL_SITES.md)
- [Route parity](v1-op-parity-buildout.md)
- [Lean refactor audit](lean-refactor-audit.md)
- [Refactor baselines](refactor-baselines.md)

## Source of truth

Generated files under `docs/gen/` come from the command registry, configuration descriptors, and
route descriptors. Change the source, then run:

```bash
make -C src docs-gen
make -C src docs-gen-check
```

`docs/proposals/` records design work. A file under `done/` explains why a feature was built; it is
not the user manual. A file under `pending/` may describe work that has not shipped. Benchmark and
validation reports preserve the conditions and results of a particular run. Current behavior lives
in the guides above and, finally, in the code and generated references.
