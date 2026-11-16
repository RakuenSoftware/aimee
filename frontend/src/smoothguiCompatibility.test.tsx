// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { ThemeToggle } from '@rakuensoftware/smoothgui';

afterEach(() => {
  cleanup();
  localStorage.clear();
  document.documentElement.removeAttribute('data-theme');
  vi.unstubAllGlobals();
});

it('keeps the application theme choice across toggle remounts using its existing storage key', () => {
  vi.stubGlobal('matchMedia', () => ({ matches: false, addEventListener: vi.fn(), removeEventListener: vi.fn() }));
  localStorage.setItem('sg-theme', 'dark');
  const first = render(<ThemeToggle />);
  expect(document.documentElement.getAttribute('data-theme')).toBe('dark');
  fireEvent.click(screen.getByRole('button', { name: 'Switch to light theme' }));
  expect(localStorage.getItem('sg-theme')).toBe('light');
  expect(document.documentElement.getAttribute('data-theme')).toBe('light');
  first.unmount();
  document.documentElement.removeAttribute('data-theme');
  render(<ThemeToggle />);
  expect(document.documentElement.getAttribute('data-theme')).toBe('light');
  expect(screen.getByRole('button', { name: 'Switch to dark theme' })).toBeTruthy();
});

it('keeps the application toggle usable when browser storage is unavailable', () => {
  vi.stubGlobal('matchMedia', () => ({ matches: false, addEventListener: vi.fn(), removeEventListener: vi.fn() }));
  const get = vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => { throw new Error('Blocked'); });
  const set = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => { throw new Error('Blocked'); });
  try {
    render(<ThemeToggle />);
    fireEvent.click(screen.getByRole('button', { name: 'Switch to dark theme' }));
    expect(document.documentElement.getAttribute('data-theme')).toBe('dark');
    expect(screen.getByRole('button', { name: 'Switch to light theme' })).toBeTruthy();
  } finally {
    get.mockRestore();
    set.mockRestore();
  }
});
