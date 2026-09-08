/** @vitest-environment jsdom */
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import ConnectWorkspace from './ConnectWorkspace';

vi.mock('@rakuensoftware/smoothgui', () => ({
  Button: ({ children, variant: _variant, ...props }: React.PropsWithChildren<{ variant?: string }>) =>
    <button {...props}>{children}</button>,
}));

afterEach(() => { cleanup(); vi.unstubAllGlobals(); vi.useRealTimers(); });

it('shows a checkout published after its clone response was lost, and excludes it from retry', async () => {
  const repo = { name: 'a', clone_url: 'https://github.com/example/a.git' };
  let published = false;
  const request = vi.fn(async (path: string) => {
    if (path === '/api/git/clone-org') {
      published = true;
      throw new TypeError('Failed to fetch');
    }
    if (path.startsWith('/api/git/org-repos')) return new Response(JSON.stringify({ repos: [repo] }));
    if (path === '/api/git/projects') return new Response(JSON.stringify({
      projects: published ? ['example/a'] : [],
      details: published ? [{ ref: 'example/a', org: 'example', name: 'a', remote: repo.clone_url }] : [],
    }));
    throw new Error(`Unexpected request: ${path}`);
  });
  vi.stubGlobal('fetch', request);
  const changed = vi.fn();
  render(<ConnectWorkspace onDone={() => {}} onProjectsChanged={changed} />);
  fireEvent.change(screen.getByPlaceholderText(/owner URL/), { target: { value: 'github.com/example' } });
  fireEvent.click(screen.getByRole('button', { name: 'List repositories' }));
  fireEvent.click(await screen.findByRole('button', { name: 'Clone selected (1)' }));

  await screen.findByText('cloned');
  expect(screen.queryByText(/No clone result|unavailable/)).toBeNull();
  expect(screen.getByText('cloned')).toBeTruthy();
  expect(screen.getByText(/Workspace has 1 project: example\/a/)).toBeTruthy();
  await waitFor(() => expect(changed).toHaveBeenLastCalledWith(1));
  expect((screen.getByRole('button', { name: 'Clone selected (0)' }) as HTMLButtonElement).disabled).toBe(true);
  expect(request.mock.calls.filter(([path]) => path === '/api/git/clone-org')).toHaveLength(1);
});

it('discovers a checkout that finishes later without a click or page reload', async () => {
  vi.useFakeTimers();
  let published = false;
  vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify({
    projects: published ? ['example/late'] : [], details: [],
  }))));
  await act(async () => {
    render(<ConnectWorkspace onDone={() => {}} />);
  });
  expect(screen.queryByText(/Workspace has/)).toBeNull();
  published = true;

  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });

  expect(screen.getByText(/Workspace has 1 project: example\/late/)).toBeTruthy();
});
