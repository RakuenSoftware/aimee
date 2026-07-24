/** @vitest-environment jsdom */
import { cleanup, render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ConsoleApp from './ConsoleApp';

const mocks = vi.hoisted(() => ({ loadSession: vi.fn() }));

vi.mock('./api', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./api')>();
  return { ...actual, loadSession: mocks.loadSession };
});

beforeEach(() => mocks.loadSession.mockReset());
afterEach(cleanup);

describe('ConsoleApp fleet mutation bootstrap', () => {
  it('restores the durable fleet lock after a full application reload', async () => {
    mocks.loadSession.mockResolvedValue({
      csrf: 'csrf-token',
      break_glass: false,
      fleet_indeterminate: true,
    });
    render(<MemoryRouter initialEntries={['/fleet']}><ConsoleApp /></MemoryRouter>);
    expect(await screen.findByText(/Further mutations are blocked for this session/)).toBeTruthy();
  });
});
