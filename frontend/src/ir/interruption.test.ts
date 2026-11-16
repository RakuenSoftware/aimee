import { describe, expect, it } from 'vitest';
import { validNativeOwner, validToolInterruption } from './interruption';
import { nativeOwner, receipt } from './interruption.fixture';
describe('typed native interruption lifecycle receipt', () => {
  it('accepts bounded exact decimal identity beyond JS integer precision', () => {
    const value = receipt();
    value.source.sequence = '9007199254740993';
    value.terminal_sequence = '9223372036854775807';
    value.owner.epoch = 'd'.repeat(32);
    expect(validToolInterruption(value)).toBe(true);
  });
  it.each(['0', '-1', '01', '1.0', '1e1', '9223372036854775808'])('refuses noncanonical or overflowing sequence %s', sequence => {
    expect(validToolInterruption({ ...receipt(), terminal_sequence: sequence })).toBe(false);
    expect(validToolInterruption({ ...receipt(), source: { ...receipt().source, sequence } })).toBe(false);
  });
  it.each([null, false, 1.5, -1, 4096])('requires a non-null canonical block index %s', block_index => {
    expect(validToolInterruption({ ...receipt(), source: { ...receipt().source, block_index } })).toBe(false);
  });
  it('refuses missing and extra members at every typed level', () => {
    for (const part of ['payload', 'source', 'owner']) {
      const value = receipt() as unknown as Record<string, unknown>;
      const target = part === 'payload' ? value : value[part] as Record<string, unknown>;
      target.untyped = {};
      expect(validToolInterruption(value)).toBe(false);
    }
    const value = receipt();
    delete (value.source as Partial<typeof value.source>).block_index;
    expect(validToolInterruption(value)).toBe(false);
    expect(validToolInterruption({ ...receipt(), owner: null })).toBe(false);
  });
  it('admits only unknown effects with explicit permission required, never success or retry authority', () => {
    expect(validToolInterruption({ ...receipt(), outcome: 'failed' })).toBe(false);
    expect(validToolInterruption({ ...receipt(), outcome: 'succeeded' })).toBe(false);
    expect(validToolInterruption({ ...receipt(), retry: 'allowed' })).toBe(false);
    expect(validToolInterruption({ ...receipt(), owner: { ...receipt().owner, owner_id: 'A'.repeat(32) } })).toBe(false);
  });
  it('enforces UTF-8 byte bounds and the full encoded payload bound', () => {
    expect(validToolInterruption({ ...receipt(), source: { ...receipt().source, actor: 'é'.repeat(64) } })).toBe(true);
    expect(validToolInterruption({ ...receipt(), source: { ...receipt().source, actor: 'é'.repeat(65) } })).toBe(false);
    expect(validToolInterruption({ ...receipt(), source: { ...receipt().source, name: '\0' } })).toBe(false);
    const value = receipt();
    for (const key of ['event_id', 'message_id', 'tool_id', 'name'] as const) value.source[key] = '\u0001'.repeat(1024);
    expect(validToolInterruption(value)).toBe(false);
  });
});

it('admits only the exact protected native snapshot owner fields', () => {
  expect(validNativeOwner(nativeOwner())).toBe(true);
  expect(validNativeOwner({ ...nativeOwner(), harness: 'claude', epoch: 'd'.repeat(32) })).toBe(true);
  for (const change of [{ version: 2 }, { harness: 'http' }, { owner_id: 'A'.repeat(32) },
    { binding_sha256: 'f'.repeat(63) }, { containment_sha256: null }, { epoch: '1' }, { extra: true }])
    expect(validNativeOwner({ ...nativeOwner(), ...change })).toBe(false);
});
