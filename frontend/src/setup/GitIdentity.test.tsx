/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import GitIdentity, { storeIdentityField } from './GitIdentity';

vi.mock('@rakuensoftware/smoothgui', () => {
  const toast = Object.assign(() => {}, { error: () => {}, success: () => {}, info: () => {} });
  return {
    Button: ({ children, ...rest }: { children?: unknown } & Record<string, unknown>) =>
      <button {...rest}>{children as never}</button>,
    useToast: () => toast,
  };
});

afterEach(cleanup);

describe('Git identity Vault writes', () => {
  it('stores only the canonical git credential name and secret', async () => {
    (window as { _csrf?: string })._csrf = 'csrf-token';
    let request: RequestInit | undefined;
    const fake = (async (path: string | URL | Request, init?: RequestInit) => {
      expect(path).toBe('/api/vault/credentials');
      request = init;
      return { ok: true, status: 200, json: async () => ({ status: 'ok' }) };
    }) as unknown as typeof fetch;

    expect(await storeIdentityField('author_email', 'operator@example.com', fake)).toBeNull();
    expect(request?.method).toBe('POST');
    expect((request?.headers as Record<string, string>)['X-CSRF-Token']).toBe('csrf-token');
    expect(JSON.parse(String(request?.body))).toEqual({
      agent: 'git', cred: 'author_email', secret: 'operator@example.com',
    });
  });

  it('returns the sanitized server error instead of treating a 2xx body as success', async () => {
    (window as { _csrf?: string })._csrf = 'csrf-token';
    const fake = (async () => ({
      ok: true, status: 200, json: async () => ({ error: '<b>vault locked</b>\n' }),
    })) as unknown as typeof fetch;
    expect(await storeIdentityField('author_name', 'Operator', fake)).toBe('b vault locked /b');
  });

  it('stores through the same-origin session when the legacy CSRF field is absent', async () => {
    delete (window as { _csrf?: string })._csrf;
    const fake = vi.fn(async (_path: RequestInfo | URL, init?: RequestInit) => {
      expect((init?.headers as Record<string, string>)['X-CSRF-Token']).toBeUndefined();
      return { ok: true, status: 200, json: async () => ({ status: 'ok' }) } as Response;
    });
    expect(await storeIdentityField('author_name', 'Operator', fake as unknown as typeof fetch))
      .toBeNull();
    expect(fake).toHaveBeenCalledOnce();
  });

  it('makes skipping explicit without writing partial identity data', () => {
    const onSkip = vi.fn();
    const fetchImpl = vi.fn();
    render(<GitIdentity onSaved={() => {}} onSkip={onSkip}
      fetchImpl={fetchImpl as unknown as typeof fetch} />);

    fireEvent.click(screen.getByRole('button', { name: 'Skip for now — commits will be refused' }));

    expect(onSkip).toHaveBeenCalledOnce();
    expect(fetchImpl).not.toHaveBeenCalled();
  });
});
