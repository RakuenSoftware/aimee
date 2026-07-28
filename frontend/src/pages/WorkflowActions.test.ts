import { describe, expect, it } from "vitest";
import { isHumanGatePause, isTerminal } from "./WorkflowActions";

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
