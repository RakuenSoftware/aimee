/** @vitest-environment jsdom */

import { afterEach, describe, expect, it, vi } from "vitest";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import WorkflowActions, { TriggersPanel, type Trigger } from "./WorkflowActions";

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
});

const baseProps = {
  triggers: [] as Trigger[],
  onChange: vi.fn(),
  version: "version-1",
  onVersion: vi.fn(),
  workflows: ["build", "manual-review"],
  workspaces: [{ label: "demo", value: "/srv/repos/demo" }],
  editable: true,
  maxRules: 32,
  registryError: "",
  loadError: "",
  open: true,
  onToggle: vi.fn(),
};

describe("TriggersPanel", () => {
  it("creates a usable trigger through the browser API contract", async () => {
    const onChange = vi.fn();
    const onVersion = vi.fn();
    const fetchMock = vi.spyOn(globalThis, "fetch").mockImplementation(async (_input, init) => {
      if (init?.method === "POST") {
        return new Response(JSON.stringify({ ok: true, key: "trigger_rules" }), {
          status: 200, headers: { "Content-Type": "application/json" },
        });
      }
      return new Response(JSON.stringify({
        editable: true,
        version: "version-2",
        triggers: [{
          source: "watch-dir", event: "docs/requests", schedule: "testing",
          mode: "interactive", template: "manual-review", workspace: "/srv/repos/demo", origin: "config",
        }],
      }), { status: 200, headers: { "Content-Type": "application/json" } });
    });

    render(<TriggersPanel {...baseProps} onChange={onChange} onVersion={onVersion} />);
    fireEvent.click(screen.getByRole("button", { name: "+ New trigger" }));
    fireEvent.change(screen.getByLabelText("Workflow"), { target: { value: "manual-review" } });
    fireEvent.change(screen.getByLabelText("Directory to watch"), { target: { value: "docs/requests" } });
    fireEvent.change(screen.getByLabelText("Branch or Git ref (optional)"), { target: { value: "testing" } });
    fireEvent.change(screen.getByLabelText("Run mode"), { target: { value: "interactive" } });
    fireEvent.click(screen.getByRole("button", { name: "Save trigger" }));

    await waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(2));
    const post = fetchMock.mock.calls[0];
    expect(post[0]).toBe("/api/workflow/config/set");
    const request = JSON.parse(String(post[1]?.body));
    expect(request).toEqual({
      key: "trigger_rules",
      previous_version: "version-1",
      value: [{
        source: "watch-dir", event: "docs/requests", schedule: "testing", mode: "interactive",
        pipeline: { template: "manual-review", workspace: "/srv/repos/demo" },
      }],
    });
    expect(onChange).toHaveBeenCalledWith(expect.arrayContaining([expect.objectContaining({ template: "manual-review" })]));
    expect(onVersion).toHaveBeenCalledWith("version-2");
    expect((await screen.findByRole("status")).textContent).toContain("Trigger created");
  });

  it("keeps invalid edits local and explains how to fix them", () => {
    const fetchMock = vi.spyOn(globalThis, "fetch");
    render(<TriggersPanel {...baseProps} />);
    fireEvent.click(screen.getByRole("button", { name: "+ New trigger" }));
    fireEvent.change(screen.getByLabelText("Directory to watch"), { target: { value: "../../outside" } });
    fireEvent.click(screen.getByRole("button", { name: "Save trigger" }));
    expect(screen.getByRole("alert").textContent).toContain("inside the repository");
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("does not offer global mutations to a non-administrator", () => {
    render(<TriggersPanel {...baseProps} editable={false} />);
    expect(screen.queryByRole("button", { name: "+ New trigger" })).toBeNull();
    expect(screen.getByText(/Only the appliance administrator/)).toBeTruthy();
  });

  it("does not rewrite config sources the Go scanner cannot preserve", () => {
    render(<TriggersPanel {...baseProps} triggers={[{
      source: "cron", event: "", schedule: "0 * * * *", mode: "autonomous",
      template: "nightly", workspace: "/srv/repos/demo", origin: "config",
    }]} />);
    expect(screen.getByRole("alert").textContent).toContain("browser writes are disabled");
    expect(screen.queryByRole("button", { name: "+ New trigger" })).toBeNull();
    expect(screen.queryByRole("button", { name: "Edit" })).toBeNull();
  });
});

describe("workflow operator capabilities", () => {
  function mockWorkflowPage(editable: boolean, configResponse?: Response, operator = editable) {
    return vi.spyOn(globalThis, "fetch").mockImplementation(async (input) => {
      const url = String(input);
      if (url === "/api/workflow/items") return Response.json({ items: [] });
      if (url === "/api/workflow/defs") return Response.json({ defs: [] });
      if (url === "/api/workflow/triggers") {
        return Response.json({ operator, editable, version: "v1", triggers: [] });
      }
      if (url === "/api/git/projects") return Response.json({ root: "/srv/repos", projects: [] });
      if (url === "/api/workflow/config") {
        return configResponse || Response.json({
          config: {
            "trigger.max_concurrent": 2,
            "trigger.scan_interval_secs": 5,
            "autonomy.auto_resume_cap_parks": true,
            "autonomy.concurrency": 5,
          },
        });
      }
      throw new Error(`unexpected fetch ${url}`);
    });
  }

  it("hides operator-only controls and makes global policy read-only for ordinary users", async () => {
    mockWorkflowPage(false);
    render(<WorkflowActions />);
    await screen.findByText(/Only the appliance administrator/);
    expect(screen.queryByText("Show all (operator)")).toBeNull();

    fireEvent.click(screen.getByText("⚙ Run policy"));
    await screen.findByText(/Read-only\. Administrator access is required/);
    for (const input of screen.getAllByRole("spinbutton")) expect((input as HTMLInputElement).disabled).toBe(true);
    for (const input of screen.getAllByRole("checkbox")) expect((input as HTMLInputElement).disabled).toBe(true);
    expect(screen.getByTitle(/0 pauses new admission/)).toBeTruthy();
    expect(screen.getByTitle(/Max autonomous runs.*Default 5/)).toBeTruthy();
  });

  it("shows a load failure instead of editable zero values", async () => {
    mockWorkflowPage(true, Response.json({ error: "policy offline" }, { status: 503 }));
    render(<WorkflowActions />);
    await screen.findByText("Show all (operator)");
    fireEvent.click(screen.getByText("⚙ Run policy"));
    expect(await screen.findByText("Could not load run policy: policy offline")).toBeTruthy();
    expect(screen.queryByRole("spinbutton")).toBeNull();
  });

  it("keeps operator run access when trigger mutation is unavailable", async () => {
    mockWorkflowPage(false, undefined, true);
    render(<WorkflowActions />);
    expect(await screen.findByText("Show all (operator)")).toBeTruthy();
    expect(screen.queryByRole("button", { name: "+ New trigger" })).toBeNull();
  });
});
