# Tool-output condensation

Tool-output condensation keeps the part of a command result that changes the next decision and
stores the full result for recovery.

It runs on canonical tool results before the next model request. It never changes the tool's exit
status, ID, timing, or pairing.

## Recognized output

Command-aware filters can remove repeated build progress, passing-test detail, dependency chatter,
and duplicate lines while preserving:

- failing tests and their diagnostics;
- compiler errors and nearby context;
- command, exit status, signal, and truncation state;
- warnings required by policy;
- the tail or summary needed to understand success;
- a pointer to the full spill.

Unknown output uses a conservative generic bound or passes through. A filter failure returns the
original bounded result.

## Spills

The full result is stored under the configured aimee home with private permissions, a content hash,
size, expiry, and job/session owner. Recovery rechecks the principal and scope. A spill path is not a
public filesystem capability.

## Safety

- off mode is byte-identical;
- ANSI/control bytes are sanitized for display without changing the stored raw evidence;
- secret redaction runs before either model context or operator display;
- condensation cannot turn a failed command into success;
- truncation and missing spill are explicit;
- retention is bounded by age and bytes.

## Observe

`aimee economizer stats` reports original, retained, spilled, recovered, and estimated token/cost
counts. Measure task outcomes and recovery rate before changing a filter.
