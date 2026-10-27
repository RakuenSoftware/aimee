/** @vitest-environment jsdom */
import { render, screen, cleanup } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { UsageCost } from './UsageCost';
afterEach(cleanup);
describe('aggregate cost knowledge', () => {
 it('renders explicitly observed zero as known zero', () => {
  render(<UsageCost subtotal={0} unknownCalls={0} complete />);
  expect(screen.getByText('$0.0000')).toBeTruthy();
  expect(screen.queryByText(/unknown|unavailable/)).toBeNull();
 });
 it('keeps a known subtotal while warning about mixed unobserved work', () => {
  render(<UsageCost subtotal={2} unknownCalls={1} complete={false} />);
  expect(screen.getByText('$2.0000 observed; 1 call with unknown usage')).toBeTruthy();
 });
 it('never presents unknown-only or unmarked legacy data as free', () => {
  const { rerender } = render(<UsageCost subtotal={0} unknownCalls={2} complete={false} />);
  expect(screen.getByText('$0.0000 observed; 2 calls with unknown usage')).toBeTruthy();
  rerender(<UsageCost subtotal={0} />);
  expect(screen.getByText('$0.0000 observed; total unavailable')).toBeTruthy();
 });
});
