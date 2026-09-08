/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import Clients from "./Clients";
vi.mock("@rakuensoftware/smoothgui", () => ({
  Button: ({ children, ...props }: any) => <button {...props}>{children}</button>,
  Panel: ({ title, children }: any) => <section><h3>{title}</h3>{children}</section>,
}));
afterEach(() => { cleanup(); vi.restoreAllMocks(); vi.unstubAllGlobals(); });
const first = { id: "a", name: "Desktop", state: "paired", expires_at: 0 };
const second = { id: "b", name: "Laptop", state: "pending", expires_at: 9999999999 };
const reply = (data: unknown, ok = true) => ({ ok, json: async () => data });
it("adds another client while retaining the paired device and avoiding deployment", async () => {
  let added = false;
  const fetcher = vi.fn(async (_path, options) => {
    if (options.method === "POST") { added = true; return reply({ ...second, bearer_token: "one-time-token", tls_port: 8743 }); }
    return reply({ clients: added ? [first, second] : [first] });
  });
  vi.stubGlobal("fetch", fetcher); render(<Clients />); await screen.findByText("Desktop");
  fireEvent.change(screen.getByLabelText("Client name"), { target: { value: "Laptop" } });
  fireEvent.click(screen.getByText("Add client"));
  await screen.findByText("Laptop"); expect(screen.getByText("Desktop")).toBeTruthy();
  expect(screen.getByText(/aimee remote set/).textContent).toContain("one-time-token");
  expect(fetcher.mock.calls.every(c => c[0] === "/api/clients")).toBe(true);
  expect(JSON.parse(fetcher.mock.calls.find(c => c[1].method === "POST")![1].body)).toEqual({ name: "Laptop" });
});
it("revokes only the selected client and honors cancellation", async () => {
  const fetcher = vi.fn().mockResolvedValue(reply({ clients: [first, second] }));
  vi.stubGlobal("fetch", fetcher); const confirm = vi.spyOn(window, "confirm").mockReturnValue(false);
  render(<Clients />); await screen.findByText("Desktop"); fireEvent.click(screen.getAllByText("Revoke")[1]);
  expect(fetcher).toHaveBeenCalledTimes(1);
  confirm.mockReturnValue(true); fireEvent.click(screen.getAllByText("Revoke")[1]);
  await waitFor(() => expect(fetcher.mock.calls.some(c => c[0] === "/api/clients/revoke")).toBe(true));
  expect(JSON.parse(fetcher.mock.calls.find(c => c[0] === "/api/clients/revoke")![1].body)).toEqual({ id: "b" });
});
it("shows authorization failures instead of claiming an empty client list", async () => {
  vi.stubGlobal("fetch", vi.fn().mockResolvedValue(reply({ error: "only the server owner can manage clients" }, false)));
  render(<Clients />); expect((await screen.findByRole("alert")).textContent).toContain("server owner");
  expect(screen.queryByText("No clients have been registered.")).toBeNull();
});
it("removes a pairing token from the screen once its device is paired", async () => {
  let created = false, paired = false;
  vi.stubGlobal("fetch", vi.fn(async (_path, options) => {
    if (options.method === "POST") { created = true; return reply({ ...second, bearer_token: "transient-token", tls_port: 8743 }); }
    return reply({ clients: created ? [{ ...second, state: paired ? "paired" : "pending" }] : [] });
  }));
  render(<Clients />); await screen.findByText("No clients have been registered.");
  fireEvent.change(screen.getByLabelText("Client name"), { target: { value: "Laptop" } }); fireEvent.click(screen.getByText("Add client"));
  await screen.findByText(/transient-token/); paired = true; fireEvent.click(screen.getByText("Refresh clients"));
  await waitFor(() => expect(screen.queryByText(/transient-token/)).toBeNull());
});
