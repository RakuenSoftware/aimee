import { describe, expect, it } from "vitest";
import { isHumanGatePause, isTerminal, proposalDraftError, triggerRulesPayload, triggerValidationError, type Trigger } from "./WorkflowActions";

describe("workflow action state compatibility", () => {
  it("shows human-gate controls for both workflow engines", () => {
    expect(isHumanGatePause("human_gate")).toBe(true);
    expect(isHumanGatePause("pending_human")).toBe(true);
    expect(isHumanGatePause("manual")).toBe(false);
  });

  it("treats an operator-stopped Go workflow as terminal", () => {
    expect(isTerminal("stopped")).toBe(true);
    expect(isTerminal("accepted")).toBe(true);
    expect(isTerminal("active")).toBe(false);
  });
});

describe("human-authored workflow triggers", () => {
  const valid: Trigger = {
    source: "watch-dir",
    event: "docs/proposals/pending",
    schedule: "testing",
    mode: "autonomous",
    template: "build",
    workspace: "/srv/repos/demo",
    origin: "config",
    max_spend_usd: 2.5,
  };

  it("validates repository confinement before saving", () => {
    expect(triggerValidationError(valid)).toBe("");
    expect(triggerValidationError({ ...valid, event: "../secrets" })).toContain("inside the repository");
    expect(triggerValidationError({ ...valid, event: "docs\\requests" })).toContain("forward slashes");
    expect(triggerValidationError({ ...valid, workspace: "" })).toContain("Choose a repository");
    expect(triggerValidationError({ ...valid, workspace: "relative/repo" })).toContain("absolute server-visible");
    expect(triggerValidationError({ ...valid, schedule: "--all" })).toContain("cannot start");
  });

  it("serializes only config-owned rules in the canonical registry shape", () => {
    const payload = triggerRulesPayload([
      valid,
      { ...valid, template: "graph-owned", origin: "workflow" },
    ]);
    expect(payload).toEqual([{
      source: "watch-dir",
      event: "docs/proposals/pending",
      schedule: "testing",
      mode: "autonomous",
      pipeline: { template: "build", workspace: "/srv/repos/demo", max_spend_usd: 2.5 },
    }]);
  });
});

describe("manual workflow submission", () => {
  it("requires the server-visible repository checkout the API needs", () => {
    const draft = { title: "Change", body: "Do the work", workflow: "build", repo: "" };
    expect(proposalDraftError(draft)).toContain("Choose a repository checkout");
    expect(proposalDraftError({ ...draft, repo: "/srv/repos/demo" })).toBe("");
  });
});
