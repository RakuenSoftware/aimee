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

it('shows native inference unavailability neutrally instead of failed or healthy', async () => {
  vi.stubGlobal('fetch', vi.fn(async (url: string) => new Response(JSON.stringify(
    url === '/api/models' ? { models: [{ name: 'native', model: 'claude', provider: 'anthropic', enabled: true }] } :
    url === '/api/models/probe' ? { name: 'native', execution_tested: false, execution_message: 'Test this native model in a conversation.' } : {},
  ))));
  render(<Models />);
  fireEvent.click(await screen.findByRole('button', { name: 'Probe', exact: true }));
  expect(await screen.findByText('Test this native model in a conversation.')).toBeTruthy();
  expect(screen.getByText('not tested', { exact: true })).toBeTruthy();
  expect(screen.queryByText('available', { exact: true })).toBeNull();
  expect(screen.queryByText('unavailable', { exact: true })).toBeNull();
});

it.each([
 [0, 0, true, '$0.0000'],
 [0, 1, false, '$0.0000 observed; 1 call with unknown usage'],
 [2, 1, false, '$2.0000 observed; 1 call with unknown usage'],
])('shows model observed subtotal %s with usage knowledge', async (cost, unknown, complete, expected) => {
 vi.stubGlobal('fetch', vi.fn(async (url: string) => new Response(JSON.stringify(
  url === '/api/models' ? { models: [{ name: 'metric-model', model: 'http', provider: 'openai', enabled: true }] } :
  url === '/api/models/stats' ? { stats: [{ name: 'metric-model', total_calls: 2, successful_calls: 2, failed_calls: 0, success_rate: 1, avg_latency_ms: 1, prompt_tokens: 0, completion_tokens: 0, estimated_cost_usd: cost, unknown_usage_calls: unknown, usage_complete: complete }] } : {},
 ))));
 render(<Models />);
 expect(await screen.findByText(expected)).toBeTruthy();
});
