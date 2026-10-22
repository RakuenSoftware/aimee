/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { BootstrapBanner } from './ChatPrimitives';
afterEach(cleanup);
it('keeps optional rules actions explicit and does not show a redundant missing-stack line', () => {
  const generate = vi.fn(), dismiss = vi.fn();
  render(<BootstrapBanner stacks={[]} onGenerate={generate} onDismiss={dismiss} />);
  expect(screen.getByRole('region', { name: 'Project rules notice' })).toBeTruthy();
  expect(screen.queryByText('No stack detected.')).toBeNull();
  expect(generate).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('button', { name: 'Dismiss', exact: true }));
  expect(dismiss).toHaveBeenCalledOnce();
  expect(generate).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('button', { name: 'Generate .aimee-rules', exact: true }));
  expect(generate).toHaveBeenCalledOnce();
});
it('retains detected stack text in an initially compact safe disclosure', () => {
  render(<BootstrapBanner stacks={[{ name: '<img src=x onerror=alert(1)>' }]} onGenerate={() => {}} onDismiss={() => {}} />);
  const summary = screen.getByText('Detected stack');
  expect(summary.closest('details')?.open).toBe(false);
  expect(screen.getByText('<img src=x onerror=alert(1)>')).toBeTruthy();
  expect(document.querySelector('img')).toBeNull();
});
