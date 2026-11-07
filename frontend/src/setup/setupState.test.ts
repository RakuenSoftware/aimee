/** @vitest-environment jsdom */
import { afterEach, expect, it } from 'vitest';
import { DISMISSED_KEY, isSetupDismissed, rememberSetupDismissal, resolveSetupDismissalScope, setDismissed } from './setupState';
afterEach(() => { setDismissed(false); localStorage.clear(); });
it('does not reuse a legacy Finish flag for an authenticated pending scope', () => {
  localStorage.setItem(DISMISSED_KEY, '1');
  expect(isSetupDismissed('a'.repeat(64))).toBe(false);
});
it('persists only an explicitly dismissed authenticated scope', () => {
  rememberSetupDismissal('a'.repeat(64));
  setDismissed(false);
  expect(isSetupDismissed('a'.repeat(64))).toBe(true);
  expect(isSetupDismissed('b'.repeat(64))).toBe(false);
});
it('keeps a close during loading in memory until its authenticated scope arrives', () => {
  rememberSetupDismissal();
  expect(isSetupDismissed()).toBe(true);
  expect(localStorage.length).toBe(0);
  resolveSetupDismissalScope('not-a-scope');
  expect(localStorage.length).toBe(0);
  resolveSetupDismissalScope('a'.repeat(64));
  setDismissed(false);
  expect(isSetupDismissed('a'.repeat(64))).toBe(true);
});
it('cannot persist pending dismissal without an admitted scope', () => {
  rememberSetupDismissal();
  resolveSetupDismissalScope();
  setDismissed(false);
  expect(localStorage.length).toBe(0);
  expect(isSetupDismissed()).toBe(false);
});
