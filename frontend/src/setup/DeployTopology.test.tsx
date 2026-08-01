/** @vitest-environment jsdom */
/* The embedder picker's safety property, exercised through the real component.
 *
 * Choosing a different embedder throws away every stored vector: same-width changes
 * because pooling and prefixes define the vector space, and different-width changes
 * because the pgvector columns themselves have to be rebuilt. The kb refuses to start
 * against a corpus embedded in another space, so a wizard that wrote the config without
 * saying so would hand the operator a deployment that will not boot. These tests pin that
 * the first choice is free, a later change is gated behind a typed confirmation, and
 * nothing reaches /api/config/set until it is confirmed.
 */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import DeployTopology from './DeployTopology';

vi.mock('@rakuensoftware/smoothgui', () => {
  const toast = Object.assign(() => {}, { error: () => {}, success: () => {}, info: () => {} });
  return {
    Button: ({ children, ...rest }: { children?: unknown } & Record<string, unknown>) => {
      const props = rest as Record<string, unknown>;
      return <button {...props}>{children as never}</button>;
    },
    useToast: () => toast,
  };
});

const EMBEDDERS = [
  { id: 'nomic-embed-text-v2-moe', dim: 768, context: 2048, pooling: 'mean', local: true, prefixed: true },
  { id: 'bekko-a25m', dim: 384, context: 8192, pooling: 'mean', local: true, prefixed: false },
  { id: 'external-only', dim: 1024, context: 512, pooling: 'mean', local: false, prefixed: false },
];

/** A fetch double over the three endpoints the page touches, recording config writes. */
function harness(config: Record<string, unknown>) {
  const writes: { key: string; value: unknown }[] = [];
  const fetchImpl = vi.fn(async (url: string, init?: RequestInit) => {
    const body = (payload: unknown) =>
      ({ ok: true, status: 200, json: async () => payload, text: async () => JSON.stringify(payload) }) as unknown as Response;
    if (url.startsWith('/api/embedders')) return body({ embedders: EMBEDDERS });
    if (url.includes('/api/config/set')) {
      writes.push(JSON.parse(String(init?.body ?? '{}')));
      return body({ ok: true });
    }
    return body({ config });
  });
  return { writes, fetchImpl };
}

async function renderPage(config: Record<string, unknown>) {
  const { writes, fetchImpl } = harness(config);
  render(<DeployTopology onSaved={() => {}} fetchImpl={fetchImpl as unknown as typeof fetch} />);
  // The picker only exists once the catalog has loaded.
  await waitFor(() => expect(screen.getByText(/bekko-a25m/)).toBeTruthy());
  return { writes };
}

function embedderSelect(): HTMLSelectElement {
  const select = Array.from(document.querySelectorAll('select')).find((s) =>
    Array.from(s.options).some((o) => o.value === 'bekko-a25m'),
  );
  if (!select) throw new Error('embedder picker not rendered');
  return select as HTMLSelectElement;
}

function save() {
  const button = Array.from(document.querySelectorAll('button')).find((b) =>
    /save & continue/i.test(b.textContent || ''),
  );
  if (!button) throw new Error('save control not found');
  fireEvent.click(button);
}

afterEach(cleanup);

describe('DeployTopology embedder picker', () => {
  it('offers only locally-hostable embedders for a local placement', async () => {
    await renderPage({});
    const values = Array.from(embedderSelect().options).map((o) => o.value);
    expect(values).toContain('nomic-embed-text-v2-moe');
    expect(values).toContain('bekko-a25m');
    // Offering one we cannot host would produce a container that refuses to boot.
    expect(values).not.toContain('external-only');
  });

  it('offers no host or GPU control at all', async () => {
    // Regression, now structural: the embed role used to render a shared placement
    // select, so an operator could pick "GPU 0" for something that runs inside the kb.
    // The choice looked real and wrote nothing. Placement is now a property of the
    // image variant, so no such control exists for either role.
    await renderPage({});
    const values = Array.from(document.querySelectorAll('select'))
      .flatMap((sel) => Array.from(sel.options).map((o) => o.value));
    expect(values.some((v) => v.startsWith('gpu:'))).toBe(false);
    expect(values).not.toContain('cpu');
    // Exactly one select carries the embedder ids, and it is the picker.
    expect(Array.from(document.querySelectorAll('select'))
      .filter((sel) => Array.from(sel.options).some((o) => o.value === 'bekko-a25m')))
      .toHaveLength(1);
  });

  it('shows the width and context, because they decide the cost of the choice', async () => {
    await renderPage({});
    const labels = Array.from(embedderSelect().options).map((o) => o.textContent || '');
    expect(labels.some((l) => l.includes('768-dim'))).toBe(true);
    expect(labels.some((l) => l.includes('384-dim') && l.includes('no prefixes'))).toBe(true);
  });

  it('seeds the shipped local embedder when config names none', async () => {
    // Regression: an unset embedder_model is not a working default. Without
    // seeding, accepting the wizard's default writes nothing, the entrypoint
    // starts no embedder, and the kb serves its builtin lexical embedder forever
    // while reporting retrieval degraded — on an image built around a real one.
    const { writes } = await renderPage({});
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedder_model')).toBe(true));
    const v = writes.find((w) => w.key === 'embedder_model')?.value;
    expect(v).toBeTruthy();
    expect(['nomic-embed-text-v2-moe', 'bekko-a25m']).toContain(v);
  });

  it('a first choice needs no confirmation (no corpus to invalidate)', async () => {
    const { writes } = await renderPage({});
    fireEvent.change(embedderSelect(), { target: { value: 'bekko-a25m' } });
    expect(screen.queryByLabelText('confirm re-embed')).toBeNull();
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedder_model')).toBe(true));
    expect(writes.find((w) => w.key === 'embedder_model')?.value).toBe('bekko-a25m');
  });

  it('a width change demands a typed confirmation and writes nothing without it', async () => {
    const { writes } = await renderPage({ embedder_model: 'nomic-embed-text-v2-moe' });
    fireEvent.change(embedderSelect(), { target: { value: 'bekko-a25m' } });
    // 768 -> 384: the columns are rebuilt too, and the warning must say so.
    await waitFor(() => expect(screen.getByLabelText('confirm re-embed')).toBeTruthy());
    expect(screen.getByText(/widths differ/i)).toBeTruthy();

    save();
    await waitFor(() => expect(screen.getByText(/Type RE-EMBED to confirm/i)).toBeTruthy());
    expect(writes.some((w) => w.key === 'embedder_model')).toBe(false);

    fireEvent.change(screen.getByLabelText('confirm re-embed'), { target: { value: 'RE-EMBED' } });
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedder_model')).toBe(true));
    expect(writes.find((w) => w.key === 'embedder_model')?.value).toBe('bekko-a25m');
  });

  it('a local choice never pins embedder_dims', async () => {
    const { writes } = await renderPage({});
    fireEvent.change(embedderSelect(), { target: { value: 'bekko-a25m' } });
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedder_model')).toBe(true));
    // The registry declares the width and the kb derives it; a second copy in config is
    // a second place to be wrong.
    expect(writes.some((w) => w.key === 'embedder_dims')).toBe(false);
  });

  it('states the one-way door at the point of choice, not in a tooltip', async () => {
    await renderPage({});
    expect(screen.getByText(/effectively permanent for this install/i)).toBeTruthy();
  });
});

describe('DeployTopology synthesis picker', () => {
  function synthSelect(): HTMLSelectElement {
    const select = Array.from(document.querySelectorAll('select')).find((s) =>
      Array.from(s.options).some((o) => o.value === 'off'),
    );
    if (!select) throw new Error('synthesis picker not rendered');
    return select as HTMLSelectElement;
  }

  it('offers off, external and THE ONE MODEL THIS IMAGE BAKES', async () => {
    // The model is in the image, so there is no menu of models to pick from — the
    // tag decided it, exactly like the embedder.
    await renderPage({ aimee_with_llamacpp: '1', aimee_synthesis_model: 'gemma-4-E4B-it' });
    const values = Array.from(synthSelect().options).map((o) => o.value);
    expect(values).toEqual(expect.arrayContaining(['off', 'external', 'gemma-4-E4B-it']));
    // The model this image does NOT carry must not be offered.
    expect(values).not.toContain('gemma-4-E2B-it');
  });

  it('offers the e2b model on an e2b image', async () => {
    await renderPage({ aimee_with_llamacpp: '1', aimee_synthesis_model: 'gemma-4-E2B-it' });
    const values = Array.from(synthSelect().options).map((o) => o.value);
    expect(values).toContain('gemma-4-E2B-it');
    expect(values).not.toContain('gemma-4-E4B-it');
  });

  it('offers no local option at all when the image bakes no model', async () => {
    // Offering a choice that cannot work is worse than not offering it: the failure
    // would only show up later as synthesis silently never starting.
    await renderPage({});
    const values = Array.from(synthSelect().options).map((o) => o.value);
    expect(values).toEqual(expect.arrayContaining(['off', 'external']));
    expect(values.some((v) => v.startsWith('gemma-'))).toBe(false);
    expect(screen.getByText(/bundles no synthesis model/i)).toBeTruthy();
  });

  it('offers no local option when llama.cpp is present but no model is', async () => {
    // Half-built image: the binary without its weights cannot serve anything.
    await renderPage({ aimee_with_llamacpp: '1' });
    const values = Array.from(synthSelect().options).map((o) => o.value);
    expect(values.some((v) => v.startsWith('gemma-'))).toBe(false);
  });

  it('off is presented as supported, and writes no endpoint', async () => {
    const { writes } = await renderPage({ synthesis_endpoint: 'https://old/v1' });
    fireEvent.change(synthSelect(), { target: { value: 'off' } });
    expect(screen.getByText(/Supported, not a gap/i)).toBeTruthy();
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'synthesis_endpoint')).toBe(true));
    expect(writes.find((w) => w.key === 'synthesis_endpoint')?.value).toBe('');
  });

  it('a bundled model writes the model and NOT a loopback endpoint', async () => {
    // The entrypoint owns the port; writing 127.0.0.1 here would hardcode it.
    const { writes } = await renderPage({
      aimee_with_llamacpp: '1', aimee_synthesis_model: 'gemma-4-E4B-it',
    });
    fireEvent.change(synthSelect(), { target: { value: 'gemma-4-E4B-it' } });
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'synthesis_model')).toBe(true));
    expect(writes.find((w) => w.key === 'synthesis_model')?.value).toBe('gemma-4-E4B-it');
    const ep = writes.find((w) => w.key === 'synthesis_endpoint');
    expect(ep === undefined || ep.value === '').toBe(true);
  });
});
