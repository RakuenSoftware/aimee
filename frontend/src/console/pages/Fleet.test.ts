import { describe, expect, it } from 'vitest';
import { canonicalTeam, validAgent } from './Fleet';

describe('fleet input contracts', () => {
  it('accepts only canonical positive signed-64-bit team ids', () => {
    expect(canonicalTeam('1')).toBe('1');
    expect(canonicalTeam('9223372036854775807')).toBe('9223372036854775807');
    for (const bad of ['', '0', '01', '+1', '-1', '1.0', '9223372036854775808']) {
      expect(canonicalTeam(bad)).toBeNull();
    }
  });

  it('matches the bounded C3 agent grammar', () => {
    expect(validAgent('agent.one-2')).toBe(true);
    expect(validAgent('')).toBe(false);
    expect(validAgent('a/b')).toBe(false);
    expect(validAgent('a'.repeat(64))).toBe(false);
  });
});
