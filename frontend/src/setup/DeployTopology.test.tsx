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
    if (url.startsWith('/api/hosts')) return body({ hosts: [{ name: 'box', kind: 'local', gpus: [] }] });
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

  it('offers NO placement for the embedder, only the model', async () => {
    // Regression: the embed role used to render the shared placement select, so an
    // operator could pick "GPU 0" for something that runs inside the kb. The choice
    // looked real and wrote nothing, which is worse than not offering it.
    await renderPage({});
    const selects = Array.from(document.querySelectorAll('select'));
    const embedderValues = Array.from(embedderSelect().options).map((o) => o.value);
    expect(embedderValues).toContain('nomic-embed-text-v2-moe');
    // No select anywhere may offer a tier/GPU option to the embedder card.
    const placementOptions = selects
      .filter((sel) => sel !== embedderSelect())
      .flatMap((sel) => Array.from(sel.options).map((o) => o.value));
    expect(placementOptions).not.toContain('gpu:0');
    // Exactly one select carries the embedder ids, and it is the picker.
    expect(selects.filter((sel) => Array.from(sel.options).some((o) => o.value === 'bekko-a25m')))
      .toHaveLength(1);
  });

  it('shows the width and context, because they decide the cost of the choice', async () => {
    await renderPage({});
    const labels = Array.from(embedderSelect().options).map((o) => o.textContent || '');
    expect(labels.some((l) => l.includes('768-dim'))).toBe(true);
    expect(labels.some((l) => l.includes('384-dim') && l.includes('no prefixes'))).toBe(true);
  });

  it('a first choice needs no confirmation (no corpus to invalidate)', async () => {
    const { writes } = await renderPage({});
    fireEvent.change(embedderSelect(), { target: { value: 'bekko-a25m' } });
    expect(screen.queryByLabelText('confirm re-embed')).toBeNull();
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedding_model')).toBe(true));
    expect(writes.find((w) => w.key === 'embedding_model')?.value).toBe('bekko-a25m');
  });

  it('a width change demands a typed confirmation and writes nothing without it', async () => {
    const { writes } = await renderPage({ embedding_model: 'nomic-embed-text-v2-moe' });
    fireEvent.change(embedderSelect(), { target: { value: 'bekko-a25m' } });
    // 768 -> 384: the columns are rebuilt too, and the warning must say so.
    await waitFor(() => expect(screen.getByLabelText('confirm re-embed')).toBeTruthy());
    expect(screen.getByText(/widths differ/i)).toBeTruthy();

    save();
    await waitFor(() => expect(screen.getByText(/Type RE-EMBED to confirm/i)).toBeTruthy());
    expect(writes.some((w) => w.key === 'embedding_model')).toBe(false);

    fireEvent.change(screen.getByLabelText('confirm re-embed'), { target: { value: 'RE-EMBED' } });
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedding_model')).toBe(true));
    expect(writes.find((w) => w.key === 'embedding_model')?.value).toBe('bekko-a25m');
  });

  it('a local choice never pins embedding_dim', async () => {
    const { writes } = await renderPage({});
    fireEvent.change(embedderSelect(), { target: { value: 'bekko-a25m' } });
    save();
    await waitFor(() => expect(writes.some((w) => w.key === 'embedding_model')).toBe(true));
    // The registry declares the width and the kb derives it; a second copy in config is
    // a second place to be wrong.
    expect(writes.some((w) => w.key === 'embedding_dim')).toBe(false);
  });
});
