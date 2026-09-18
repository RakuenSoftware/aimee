/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ProjectPicker from './ProjectPicker';

beforeEach(() => { localStorage.clear(); });
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });

describe('session project picker', () => {
  it('loads options without clearing the server-owned selection or restoring a stale browser selection', async () => {
    localStorage.setItem('aimee_session_project_one', 'old-project');
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({
      root: '/work', projects: ['old-project', 'org/current-project'],
    }))));
    const onChange = vi.fn();
    const view = render(<ProjectPicker value="/work/org/current-project" onChange={onChange} />);
    await screen.findByRole('option', { name: 'org/current-project' });
    expect((screen.getByRole('combobox') as HTMLSelectElement).value).toBe('/work/org/current-project');
    expect(onChange).not.toHaveBeenCalled();
    view.rerender(<ProjectPicker value="/work/old-project" onChange={onChange} />);
    expect((screen.getByRole('combobox') as HTMLSelectElement).value).toBe('/work/old-project');
    expect(onChange).not.toHaveBeenCalled();
    fireEvent.change(screen.getByRole('combobox'), { target: { value: '/work/org/current-project' } });
    expect(onChange).toHaveBeenCalledExactlyOnceWith({ project: 'org/current-project', root: '/work' });
  });

  it('does not clear the binding when a project list is temporarily empty', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({ root: '/work', projects: [] }))));
    const onChange = vi.fn();
    render(<ProjectPicker value="/work/existing-project" onChange={onChange} />);
    await waitFor(() => expect(fetch).toHaveBeenCalledOnce());
    expect(onChange).not.toHaveBeenCalled();
  });

  it('selects a cloned project using the root returned by the reload', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(new Response(JSON.stringify({ root: '/old-workspace', projects: [] })))
      .mockResolvedValueOnce(new Response(JSON.stringify({ name: 'org/new-project' })))
      .mockResolvedValueOnce(new Response(JSON.stringify({ root: '/new-workspace', projects: ['org/new-project'] }))));
    const onChange = vi.fn();
    render(<ProjectPicker value="" onChange={onChange} />);
    await waitFor(() => expect(fetch).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: '+ Clone repo' }));
    fireEvent.change(screen.getByPlaceholderText('git remote URL (https or ssh)'), { target: { value: 'https://example.com/org/new-project' } });
    fireEvent.click(screen.getByRole('button', { name: 'Clone' }));
    await waitFor(() => expect(onChange).toHaveBeenCalledExactlyOnceWith({ project: 'org/new-project', root: '/new-workspace' }));
  });
});
