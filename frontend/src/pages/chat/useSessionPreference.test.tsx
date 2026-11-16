/** @vitest-environment jsdom */
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { useSessionPreference } from './useSessionPreference';

afterEach(() => { cleanup(); vi.unstubAllGlobals(); });
function Preference({ session }: { session: string }) {
  const setting = useSessionPreference(session, '/api/chat/primary', 'agent');
  return <><output data-known={setting.known}>{setting.value || 'default'}</output><button onClick={() => setting.change('new')}>New</button>
    <button onClick={() => setting.change('last')}>Last</button>{setting.error && <p role="alert">{setting.error}</p>}</>;
}
const response = (agent: string, status = 200) => new Response(JSON.stringify({ agent }), { status });
function deferred() { let resolve!: (value: Response) => void; const promise = new Promise<Response>(r => { resolve = r; }); return { promise, resolve }; }

it('ignores an old-session selection read after navigation', async () => {
  const old = deferred();
  vi.stubGlobal('fetch', vi.fn((url: string) => url.endsWith('sid=one') ? old.promise : Promise.resolve(response('two-model'))));
  const view = render(<Preference session="one" />);
  view.rerender(<Preference session="two" />);
  await act(async () => {});
  expect(screen.getByRole('status').textContent).toBe('two-model');
  await act(async () => { old.resolve(response('one-model')); });
  expect(screen.getByRole('status').textContent).toBe('two-model');
});

it('does not let an initial history read undo a newer selection', async () => {
  const initial = deferred();
  vi.stubGlobal('fetch', vi.fn((_url, init) => init?.method === 'POST' ? Promise.resolve(response('new')) : initial.promise));
  render(<Preference session="one" />);
  fireEvent.click(screen.getByText('New'));
  await act(async () => {});
  await act(async () => { initial.resolve(response('old')); });
  expect(screen.getByRole('status').textContent).toBe('new');
});

it('serializes rapid changes and rolls a refused latest choice back to the last saved choice', async () => {
  const first = deferred(); const last = deferred();
  const fetcher = vi.fn((_url, init) => !init?.method ? Promise.resolve(response('initial')) : JSON.parse(init.body).agent === 'new' ? first.promise : last.promise);
  vi.stubGlobal('fetch', fetcher);
  render(<Preference session="one" />);
  await act(async () => {});
  fireEvent.click(screen.getByText('New')); fireEvent.click(screen.getByText('Last'));
  await act(async () => {});
  expect(fetcher.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
  expect(screen.getByRole('status').textContent).toBe('last');
  await act(async () => { first.resolve(response('new')); });
  expect(fetcher.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(2);
  await act(async () => { last.resolve(response('', 409)); });
  expect(screen.getByRole('status').textContent).toBe('new');
  expect(screen.getByRole('alert').textContent).toContain('Could not save');
});

it('keeps a failed old-session write from reverting the newly selected session', async () => {
  const write = deferred();
  vi.stubGlobal('fetch', vi.fn((url: string, init) => init?.method ? write.promise : Promise.resolve(response(url.endsWith('sid=one') ? 'one-model' : 'two-model'))));
  const view = render(<Preference session="one" />);
  await act(async () => {});
  fireEvent.click(screen.getByText('New'));
  view.rerender(<Preference session="two" />);
  await act(async () => {});
  await act(async () => { write.resolve(response('', 409)); });
  expect(screen.getByRole('status').textContent).toBe('two-model');
  expect(screen.queryByRole('alert')).toBeNull();
});

it('retains a saved pin and reports refresh failure instead of inventing the default', async () => {
  const fetcher = vi.fn().mockResolvedValue(response('saved-model'));
  vi.stubGlobal('fetch', fetcher);
  render(<Preference session="one" />);
  await act(async () => {});
  fetcher.mockResolvedValue(response('', 503));
  await act(async () => { window.dispatchEvent(new Event('focus')); });
  expect(screen.getByRole('status').textContent).toBe('saved-model');
  expect(screen.getByRole('alert').textContent).toContain('last saved selection');
});
it('refreshes another device’s pin on focus and ignores older overlapping reads', async () => {
  const initial = deferred(); const latest = deferred();
  vi.stubGlobal('fetch', vi.fn().mockReturnValueOnce(initial.promise).mockReturnValueOnce(latest.promise));
  render(<Preference session="one" />);
  await act(async () => { window.dispatchEvent(new Event('focus')); });
  await act(async () => { latest.resolve(response('another-device-model')); });
  await act(async () => { initial.resolve(response('outdated-model')); });
  expect(screen.getByRole('status').textContent).toBe('another-device-model');
});
it('does not let a focus refresh replace an optimistic selection with stale server state', async () => {
  const refresh = deferred(); const save = deferred();
  vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(response('initial')).mockReturnValueOnce(refresh.promise).mockReturnValueOnce(save.promise));
  render(<Preference session="one" />);
  await act(async () => {});
  await act(async () => { window.dispatchEvent(new Event('focus')); });
  fireEvent.click(screen.getByText('New'));
  await act(async () => { refresh.resolve(response('outdated-model')); });
  expect(screen.getByRole('status').textContent).toBe('new');
  await act(async () => { save.resolve(response('new')); });
  expect(screen.getByRole('status').textContent).toBe('new');
});

it.each([{}, { agent: null }, { agent: 17 }])('keeps malformed initial replies unknown: %j', async body => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify(body))));
  render(<Preference session="one" />);
  await act(async () => {});
  expect(screen.getByRole('status').getAttribute('data-known')).toBe('false');
  expect(screen.getByRole('alert').textContent).toContain('Current selection is unavailable');
});
it.each([{}, { agent: null }, { agent: 17 }])('preserves the confirmed pin after malformed refresh: %j', async body => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(response('confirmed-model'))
    .mockResolvedValueOnce(new Response(JSON.stringify(body))));
  render(<Preference session="one" />);
  await act(async () => {});
  await act(async () => { window.dispatchEvent(new Event('focus')); });
  expect(screen.getByRole('status').textContent).toBe('confirmed-model');
  expect(screen.getByRole('status').getAttribute('data-known')).toBe('true');
  expect(screen.getByRole('alert').textContent).toContain('last saved selection');
});
it('accepts an explicit empty pin as a known default', async () => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response('')));
  render(<Preference session="one" />);
  await act(async () => {});
  expect(screen.getByRole('status').getAttribute('data-known')).toBe('true');
  expect(screen.getByRole('status').textContent).toBe('default');
  expect(screen.queryByRole('alert')).toBeNull();
});
