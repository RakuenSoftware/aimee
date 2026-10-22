/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import Models from './Models';

afterEach(() => { cleanup(); vi.unstubAllGlobals(); });

it('names the real model editor from its visible title and closes without saving', async () => {
  const fetchMock = vi.fn(async (url: string, _init?: RequestInit) => new Response(JSON.stringify(
    url === '/api/models' ? { models: [{ name: 'owned-http-model', registration: 'owned-provider', model: 'offered-model', provider: 'chatgpt', endpoint: 'https://example.test/codex', enabled: true }] } : {},
  )));
  vi.stubGlobal('fetch', fetchMock);
  render(<Models />);
  fireEvent.click(await screen.findByRole('button', { name: 'Edit', exact: true }));
  const dialog = screen.getByRole('dialog', { name: 'Edit model', exact: true });
  expect(within(dialog).getByText('Edit model', { exact: true })).toBeTruthy();
  expect(within(dialog).getByText('owned-http-model', { exact: true })).toBeTruthy();
  expect(within(dialog).getByLabelText('model', { exact: true })).toHaveProperty('value', 'offered-model');
  const titleId = dialog.getAttribute('aria-labelledby');
  expect(titleId).toBeTruthy();
  expect(document.getElementById(titleId!)?.textContent).toBe('Edit model');
  fireEvent.click(within(dialog).getByRole('button', { name: 'Cancel', exact: true }));
  expect(screen.queryByRole('dialog')).toBeNull();
  expect(fetchMock.mock.calls.every(([, init]) => !init?.method || init.method === 'GET')).toBe(true);
});
