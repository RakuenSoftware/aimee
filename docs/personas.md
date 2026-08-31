# Personas

A persona is a named review or working perspective. It changes instructions and preferences, not
capabilities, credentials, tools, or workspace authority.

Built-in personas cover engineering, architecture, security, QA, review, roundtable chairing,
contrarian review, and technical writing. List the running catalog instead of relying on a copied
count.

## Contents

A persona can define:

- name and short purpose;
- principles and review lens;
- expected evidence and output shape;
- role preferences;
- optional model/agent routing hints.

Keep it specific. “Be thorough” is not a persona. “Check trust-boundary changes, negative auth tests,
and secret handling” is.

## Use

```bash
aimee delegate review --persona security "Review the current diff"
aimee delegate code --persona engineer "Implement this accepted slice"
```

Delegate commands require a persona where the help says so. A persona used with a write role still
needs normal write authority.

## Roundtables

Roundtable presets assign personas to seats. The seat then pins a model or selects a random eligible
review agent. The persona is the lens; the seat/model is the identity that ran it.

Workflow `gate.roundtable` names a preset and can require particular personas. If the panel cannot be
formed, the gate parks rather than lowering quorum silently.

When a preset enables its chairman, the workflow uses the built-in `chairman` persona for the final
synthesis and artifact-alignment verdict. The chairman remains a read-only reviewer; choosing a
chairman model does not grant that model the `review` role.

## Custom personas

Store custom persona files in the configured persona directory and manage them through the browser
or typed persona API where available. Validate the name, keep files private when they contain
organization policy, and version changes that affect workflow review.

Do not put credentials, private user data, or unbounded project context in a persona.

## Design rules

- one clear lens per persona;
- observable evidence requirements;
- no claims of authority the runtime does not grant;
- no provider-specific tricks unless the persona is explicitly provider-bound;
- stable names for workflow and roundtable references;
- short enough to leave room for the actual task and evidence.

See [Delegates](DELEGATES.md) and [Roundtables](ENSEMBLE.md).
