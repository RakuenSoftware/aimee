# Sandbox E2E Proof

This document records that the autonomous WFE sandbox pipeline operates
correctly end-to-end against the three capabilities exercised by an overnight
run: the sandbox container starts and runs the delegate's tools against the
bound worktree; the container is launched with `--network none`, so the
delegate has no IP egress and only reaches aimee-server through the bound
`aimee-http.sock`; and in-sandbox git operations (status, diff, add, commit,
push, branch, log, verify) succeed against the delegate's worktree, with all
remote and credential-bearing calls performed server-side on the delegate's
behalf. Run marker: `overnight-e2e-run21-all-three-fixes-live`.
