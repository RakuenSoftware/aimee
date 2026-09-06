/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import Providers from "./Providers";
import Models from "./Models";
import AddProviderModel from "../providers/AddProviderModel";

vi.mock("@rakuensoftware/smoothgui", () => ({
  Button: ({ children, variant: _v, ...props }: any) => <button {...props}>{children}</button>,
  Panel: ({ title, children }: any) => <section><h3>{title}</h3>{children}</section>,
  Modal: ({ title, children }: any) => <div role="dialog"><h3>{title}</h3>{children}</div>,
  Badge: ({ label }: any) => <span>{label}</span>,
  Spinner: () => null,
  KeyValue: ({ label, value }: any) => <div>{label}: {value}</div>,
  InlineStatus: ({ status }: any) => status ? <p>{status.msg}</p> : null,
  EmptyState: ({ message }: any) => <p>{message}</p>,
}));
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });
const work = { name: "work", provider: "openai", endpoint: "https://work/v1", auth_type: "bearer", model_count: 2 };
const reply = (data: unknown, ok = true) => ({ ok, status: ok ? 200 : 400, json: async () => data });

it("adds a second provider without asking for a model", async () => {
  const fetcher = vi.fn().mockResolvedValue(reply({ providers: [work] }));
  vi.stubGlobal("fetch", fetcher);
  render(<Providers />);
  await screen.findByText("work");
  expect(fetcher.mock.calls[0][0]).toBe("/api/providers");
  fireEvent.click(screen.getByText("+ Add provider"));
  expect(screen.queryByLabelText(/model/i)).toBeNull();
  fireEvent.change(screen.getByLabelText("Provider name"), { target: { value: "personal" } });
  fireEvent.change(screen.getByLabelText("Endpoint"), { target: { value: "https://personal/v1" } });
  fireEvent.click(screen.getByText("Save provider"));
  await waitFor(() => expect(screen.queryByRole("dialog")).toBeNull());
  const call = fetcher.mock.calls.find(c => c[0] === "/api/providers/save")!;
  expect(JSON.parse(call[1].body)).toEqual({ name: "personal", provider: "openai", endpoint: "https://personal/v1", auth_type: "bearer", create: true });
});

it("edits a connection while preserving an unchanged key", async () => {
  const fetcher = vi.fn().mockResolvedValue(reply({ providers: [work] }));
  vi.stubGlobal("fetch", fetcher);
  render(<Providers />);
  fireEvent.click(await screen.findByText("Edit provider"));
  fireEvent.change(screen.getByLabelText("Endpoint"), { target: { value: "https://new/v1" } });
  fireEvent.click(screen.getByText("Save provider"));
  await screen.findByText("Provider saved");
  const body = JSON.parse(fetcher.mock.calls.find(c => c[0] === "/api/providers/save")![1].body);
  expect(body).toEqual({ name: "work", provider: "openai", endpoint: "https://new/v1", auth_type: "bearer", create: false });
});

it("requires explicit deletion confirmation and keeps failed deletes visible", async () => {
  const fetcher = vi.fn(async (path: string) => path === "/api/providers/remove" ? reply({ error: "Save failed" }, false) : reply({ providers: [work] }));
  vi.stubGlobal("fetch", fetcher);
  render(<Providers />);
  fireEvent.click(await screen.findByText("Delete provider"));
  expect(screen.getByText(/2 attached model/)).toBeTruthy();
  fireEvent.click(screen.getByText("Cancel"));
  expect(fetcher).toHaveBeenCalledTimes(1);
  fireEvent.click(screen.getByText("Delete provider"));
  fireEvent.click(screen.getByText("Confirm delete"));
  await waitFor(() => expect(screen.getAllByText("Save failed").length).toBeGreaterThan(0));
  expect(screen.getByRole("dialog")).toBeTruthy();
  expect(screen.getByText("work")).toBeTruthy();
});

it("adds a model using the selected saved provider without resubmitting credentials", async () => {
  const fetcher = vi.fn().mockResolvedValue(reply({ providers: [work, { ...work, name: "personal" }] }));
  vi.stubGlobal("fetch", fetcher);
  const onDone = vi.fn();
  render(<AddProviderModel onDone={onDone} />);
  await screen.findByLabelText("Provider");
  fireEvent.change(screen.getByLabelText("Provider"), { target: { value: "personal" } });
  fireEvent.change(screen.getByLabelText("Model ID"), { target: { value: "example-model" } });
  fireEvent.click(screen.getByText("Add model"));
  await waitFor(() => expect(onDone).toHaveBeenCalledWith("Added example-model", true));
  const body = JSON.parse(fetcher.mock.calls.find(c => c[0] === "/api/models/add")![1].body);
  expect(body.args).toEqual(["personal:example-model", "https://work/v1", "example-model", "--registration", "personal"]);
});

it("shows load errors instead of claiming there are no providers", async () => {
  vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new Error("Service unavailable")));
  render(<Providers />);
  await screen.findByText("Service unavailable");
  expect(screen.queryByText(/No providers configured/)).toBeNull();
});

it("keeps model limits and price declarations on Models", async () => {
  const model = { name: "work:example", registration: "work", provider: "openai", model: "example", endpoint: work.endpoint, enabled: true,
    context_window: 10000, max_output: 2000, price_in_per_mtok: 0, price_in_declared: true };
  const fetcher = vi.fn(async (path: string) => reply(path === "/api/models" ? { models: [model] } : { status: "ok" }));
  vi.stubGlobal("fetch", fetcher);
  render(<Models />);
  fireEvent.click(await screen.findByText("Edit"));
  const input = screen.getByLabelText("input price ($/Mtok, blank = not stated)") as HTMLInputElement;
  expect(input.value).toBe("0");
  fireEvent.change(input, { target: { value: "" } });
  fireEvent.change(screen.getByLabelText("max output (tokens, blank = auto)"), { target: { value: "4096" } });
  fireEvent.click(screen.getByText("Save"));
  await waitFor(() => expect(screen.queryByRole("dialog")).toBeNull());
  const args = JSON.parse((fetcher.mock.calls as unknown as [string, RequestInit][]).find(c => c[0] === "/api/models/set")![1].body as string).args;
  expect(args[args.indexOf("--max-output") + 1]).toBe("4096");
  expect(args[args.indexOf("--price-in") + 1]).toBe("");
  expect(args).not.toContain("--endpoint");
});

it("discovers models from the selected connection", async () => {
  const fetcher = vi.fn(async (path: string) => reply(path === "/api/providers/models" ? { details: [{ id: "offered-model" }] } : { providers: [work] }));
  vi.stubGlobal("fetch", fetcher);
  render(<AddProviderModel onDone={() => {}} />);
  fireEvent.click(await screen.findByText("Show models this provider offers"));
  fireEvent.click(await screen.findByText("offered-model"));
  expect((screen.getByLabelText("Model ID") as HTMLInputElement).value).toBe("offered-model");
  expect(screen.getByText("context not published")).toBeTruthy();
});
