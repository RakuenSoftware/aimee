import { describe, expect, it, vi } from 'vitest';
import { cloneRepos } from './cloneRepos';

const owner = { host: 'github.com', owner: 'example' };
const repos = ['a', 'b', 'c'].map(name => ({ name, clone_url: `https://github.com/example/${name}.git` }));
const reply = (data: unknown, status = 200) => new Response(JSON.stringify(data), { status });

describe('Repository clone progress and recovery', () => {
  it('finishes and refreshes each repository before starting the next request', async () => {
    const events: string[] = [];
    const request = vi.fn(async (_path: string, init?: RequestInit) => {
      const body = JSON.parse(String(init?.body));
      expect(body.host).toBe(owner.host);
      expect(body.owner).toBe(owner.owner);
      expect(body.repos).toHaveLength(1);
      const name = body.repos[0].name;
      events.push(`request:${name}`);
      return reply({ results: [{ name, ok: true, project: `example/${name}` }] });
    });

    expect(await cloneRepos(owner, repos, request,
      result => { events.push(`result:${result.name}`); },
      async () => { events.push('refresh'); })).toBeNull();

    expect(events).toEqual(['request:a', 'result:a', 'refresh', 'request:b', 'result:b', 'refresh', 'request:c', 'result:c', 'refresh']);
  });

  it('retains completed results and refreshes after a lost response without resubmitting', async () => {
    const request = vi.fn()
      .mockResolvedValueOnce(reply({ results: [{ name: 'a', ok: true }] }))
      .mockRejectedValueOnce(new TypeError('Failed to fetch'));
    const result = vi.fn();
    const refresh = vi.fn(async () => {});

    const error = await cloneRepos(owner, repos, request, result, refresh, { recoveryAttempts: 1 });

    expect(error).toContain('Could not confirm the clone of b');
    expect(error).toContain('queue is paused');
    expect(request).toHaveBeenCalledTimes(2);
    expect(result).toHaveBeenCalledExactlyOnceWith({ name: 'a', ok: true });
    expect(refresh).toHaveBeenCalledTimes(2);
  });

  it.each([
    [503, { error: 'git: aimee-server unavailable' }],
    [504, { error: 'timeout' }],
    [200, { results: [] }],
    [200, { results: [{ name: 'different-repo', ok: true }] }],
  ])('refreshes inventory when status %s carries no reliable clone outcome', async (status, data) => {
    const request = vi.fn(async () => reply(data, status));
    const result = vi.fn();
    const refresh = vi.fn(async () => {});

    expect(await cloneRepos(owner, repos, request, result, refresh, { recoveryAttempts: 1 })).toContain('Completed repositories will continue to appear');
    expect(result).not.toHaveBeenCalled();
    expect(request).toHaveBeenCalledOnce();
    expect(refresh).toHaveBeenCalledOnce();
  });

  it('reports a confirmed repository failure and continues the remaining selection', async () => {
    const request = vi.fn()
      .mockResolvedValueOnce(reply({ results: [{ name: 'a', ok: false, error: 'repository not found' }] }))
      .mockResolvedValueOnce(reply({ results: [{ name: 'b', ok: true }] }));
    const result = vi.fn();

    expect(await cloneRepos(owner, repos.slice(0, 2), request, result, async () => {})).toBeNull();
    expect(result.mock.calls.map(([r]) => r.ok)).toEqual([false, true]);
  });

  it('waits for a late publication after a lost response, then continues without cloning it twice', async () => {
    const request = vi.fn()
      .mockRejectedValueOnce(new TypeError('Failed to fetch'))
      .mockResolvedValueOnce(reply({ results: [{ name: 'b', ok: true }] }));
    const refresh = vi.fn()
      .mockResolvedValueOnce({ projects: [], details: [] })
      .mockResolvedValue({ projects: ['example/a'], details: [
        { ref: 'example/a', remote: 'https://github.com/EXAMPLE/a' },
      ] });
    const result = vi.fn();
    const progress = vi.fn();

    expect(await cloneRepos(owner, repos.slice(0, 2), request, result, refresh,
      { onProgress: progress, recoveryDelayMs: 0 })).toBeNull();

    expect(request.mock.calls.map(([, init]) => JSON.parse(init.body).repos[0].name)).toEqual(['a', 'b']);
    expect(result.mock.calls.map(([r]) => r.project || r.name)).toEqual(['example/a', 'b']);
    expect(progress.mock.calls.map(([p]) => p && `${p.phase}:${p.name}`))
      .toEqual(['cloning:a', 'checking:a', 'cloning:b', null]);
  });

  it('does not treat a matching name from another remote as a successful clone', async () => {
    const request = vi.fn().mockRejectedValue(new TypeError('Failed to fetch'));
    const refresh = vi.fn().mockResolvedValue({ details: [
      { ref: 'other/a', remote: 'https://github.com/other/a.git' },
    ] });
    const result = vi.fn();

    expect(await cloneRepos(owner, repos, request, result, refresh, { recoveryAttempts: 1 }))
      .toContain('Could not confirm');
    expect(result).not.toHaveBeenCalled();
    expect(request).toHaveBeenCalledOnce();
  });
});
