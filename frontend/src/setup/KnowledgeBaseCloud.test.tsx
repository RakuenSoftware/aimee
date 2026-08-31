/** @vitest-environment jsdom */
/* The aimee cloud path through the knowledge-base step.
 *
 * A setup code is single-use and expires in 24 hours, so the cost of getting
 * this wrong is not a retry — it is a customer with a dead code who has to ask
 * for another. These pin the properties that protect them: the code is only
 * spent when the operator asks, a rejection does not clear the box, and nothing
 * is written to config until an exchange has actually succeeded.
 */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import KnowledgeBase, { DEFAULT_CLOUD_ENDPOINT } from './KnowledgeBase';

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

type Call = { url: string; body: unknown };

/** A fetch stub that answers /api/config, /api/config/set and the redeem
 *  endpoint, and records everything it was asked. */
function stubFetch(redeem: { status: number; body: unknown }) {
  const calls: Call[] = [];
  const impl = (async (url: string, init?: RequestInit) => {
    const body = init?.body != null ? JSON.parse(String(init.body)) : undefined;
    calls.push({ url: String(url), body });
    if (String(url).endsWith('/api/config')) {
      return { ok: true, json: async () => ({ config: {} }) } as unknown as Response;
    }
    if (String(url).endsWith('/api/config/set')) {
      return { ok: true, status: 200, json: async () => ({ value: body?.value }) } as unknown as Response;
    }
    return {
      ok: redeem.status < 400,
      status: redeem.status,
      json: async () => redeem.body,
    } as unknown as Response;
  }) as unknown as typeof fetch;
  return { impl, calls };
}

const OK_REDEEM = {
  status: 200,
  body: { kb_url: 'https://api.aimee.rakuensoftware.com', bearer: 'aik_abc_def', tenant: 'acme-co' },
};

async function openCloud(stub: ReturnType<typeof stubFetch>) {
  const onSaved = vi.fn();
  render(<KnowledgeBase onSaved={onSaved} fetchImpl={stub.impl} />);
  await screen.findByText(/aimee cloud/i);
  fireEvent.click(screen.getByLabelText(/aimee cloud/i, { selector: 'input' }));
  return onSaved;
}

beforeEach(() => {
  vi.stubGlobal('fetch', undefined);
});
afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
});

describe('aimee cloud setup code', () => {
  it('offers aimee cloud without displacing the local default', async () => {
    const stub = stubFetch(OK_REDEEM);
    render(<KnowledgeBase onSaved={vi.fn()} fetchImpl={stub.impl} />);
    expect(await screen.findByText(/aimee cloud/i)).toBeTruthy();
    // aimee is self-hostable and local stays the recommended default; the
    // installer of an AGPL project should not steer people at a paid service.
    expect(screen.getByText(/Deploy a local knowledge base \(recommended\)/i)).toBeTruthy();
    const local = screen.getByLabelText(/Deploy a local knowledge base/i, { selector: 'input' }) as HTMLInputElement;
    expect(local.checked).toBe(true);
  });

  it('asks for a code and nothing else', async () => {
    const stub = stubFetch(OK_REDEEM);
    await openCloud(stub);
    // One input on the cloud path: the code. No URL, no token, no endpoint.
    expect(screen.getByPlaceholderText(/AIMEE-/)).toBeTruthy();
    expect(screen.queryByPlaceholderText(/https:\/\/kb\.example/)).toBeNull();
  });

  it('does not spend the code until the operator asks', async () => {
    const stub = stubFetch(OK_REDEEM);
    await openCloud(stub);
    fireEvent.change(screen.getByPlaceholderText(/AIMEE-/), {
      target: { value: 'AIMEE-ABCD-ABCD-ABCD-ABCD-ABCD' },
    });
    // Typing must not redeem: the code is single-use.
    expect(stub.calls.some((c) => c.url.includes('/v1/setup/redeem'))).toBe(false);
  });

  it('exchanges the code and saves the config it returns', async () => {
    const stub = stubFetch(OK_REDEEM);
    const onSaved = await openCloud(stub);
    vi.stubGlobal('fetch', stub.impl);

    fireEvent.change(screen.getByPlaceholderText(/AIMEE-/), {
      target: { value: ' aimee-abcd-abcd-abcd-abcd-abcd ' },
    });
    fireEvent.click(screen.getByRole('button', { name: /Redeem code/i }));
    await screen.findByText(/Connected to acme-co/i);

    const redeem = stub.calls.find((c) => c.url.includes('/v1/setup/redeem'));
    expect(redeem?.url).toBe(`${DEFAULT_CLOUD_ENDPOINT}/v1/setup/redeem`);
    // Trimmed, so a pasted code with stray whitespace still works.
    expect((redeem?.body as { code: string }).code).toBe('aimee-abcd-abcd-abcd-abcd-abcd');

    fireEvent.click(screen.getByRole('button', { name: /Save & continue/i }));
    await waitFor(() => expect(onSaved).toHaveBeenCalled());

    const saved = new Map(
      stub.calls
        .filter((c) => c.url.endsWith('/api/config/set'))
        .map((c) => [(c.body as { key: string }).key, (c.body as { value: string }).value]),
    );
    expect(saved.get('kb_mode')).toBe('remote');
    expect(saved.get('kb_client_url')).toBe('https://api.aimee.rakuensoftware.com');
    expect(saved.get('kb_client_bearer_token')).toBe('aik_abc_def');
    // Cloud persists exactly what the manual remote path would.
    expect(onSaved.mock.calls[0][1]).toBe('remote');
  });

  it('cannot be saved before a code has been exchanged', async () => {
    const stub = stubFetch(OK_REDEEM);
    await openCloud(stub);
    const save = screen.getByRole('button', { name: /Save & continue/i }) as HTMLButtonElement;
    expect(save.disabled).toBe(true);
  });

  it('reports the provider\'s refusal and keeps the code for a retry', async () => {
    const stub = stubFetch({
      status: 400,
      body: { error: 'that code is not valid. Setup codes are single-use and expire after 24 hours' },
    });
    await openCloud(stub);
    vi.stubGlobal('fetch', stub.impl);

    const input = screen.getByPlaceholderText(/AIMEE-/) as HTMLInputElement;
    fireEvent.change(input, { target: { value: 'AIMEE-WRONG-WRONG-WRONG-WRONG-WRONG' } });
    fireEvent.click(screen.getByRole('button', { name: /Redeem code/i }));

    await screen.findByText(/single-use and expire/i);
    // The code stays put: a typo should be correctable without asking for a new one.
    expect(input.value).toBe('AIMEE-WRONG-WRONG-WRONG-WRONG-WRONG');
    // And nothing was written.
    expect(stub.calls.some((c) => c.url.endsWith('/api/config/set'))).toBe(false);
  });

  it('refuses a response missing the fields it needs', async () => {
    const stub = stubFetch({ status: 200, body: { tenant: 'acme-co' } });
    await openCloud(stub);
    vi.stubGlobal('fetch', stub.impl);
    fireEvent.change(screen.getByPlaceholderText(/AIMEE-/), { target: { value: 'AIMEE-X' } });
    fireEvent.click(screen.getByRole('button', { name: /Redeem code/i }));
    await screen.findByText(/did not return a usable knowledge base/i);
    expect(stub.calls.some((c) => c.url.endsWith('/api/config/set'))).toBe(false);
  });
});
