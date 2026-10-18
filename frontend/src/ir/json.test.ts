import { describe, expect, it } from 'vitest';
import { ExactJSONNumber, parseJSON, stringifyJSON } from './json';
describe('canonical opaque JSON', () => {
  it('retains large integers, decimals, extreme exponents and negative zero', () => {
    const source = '{"id":9007199254740993,"decimal":0.12345678901234567890123456789,"huge":1e999,"tiny":-1e-999,"zero":-0.00,"safe":0.125}';
    const value = parseJSON(source) as Record<string, unknown>;
    expect(value.id).toBeInstanceOf(ExactJSONNumber);
    expect(value.safe).toBe(0.125);
    expect(stringifyJSON(value)).toBe(source);
    expect(() => JSON.stringify(value)).toThrow('require stringifyJSON');
    expect(stringifyJSON(parseJSON(stringifyJSON(value, 2)!))).toBe(source);
  });
  it('retains normal envelope fields and rejects malformed JSON', () => {
    const source = '{"version":1,"usage":123,"ratio":0.1,"input":{"__proto__":{"admin":true}},"ok":true,"empty":null}';
    expect(stringifyJSON(parseJSON(source))).toBe(source);
    expect(({} as { admin?: boolean }).admin).toBeUndefined();
    for (const input of ['', '01', '1.', '1e', '1+2', '[1,]', '{"x":1,}', '"\\q"', '"a\nb"', '{} trailing', '[true false]', '{"x":1,"x":2}', '{"tool":{"a":1,"a":2}}'])
      expect(() => parseJSON(input), input).toThrow();
    expect(() => parseJSON('['.repeat(514) + '0' + ']'.repeat(514))).toThrow();
  });
  it('preserves decimal value normalization without widening known numeric fields', () => {
    expect(parseJSON('1.000')).toBe(1);
    expect(parseJSON('1e2')).toBe(100);
    expect(parseJSON('9007199254740992')).toBeInstanceOf(ExactJSONNumber);
    expect(stringifyJSON(parseJSON('1e9999999999999999999999999'))).toBe('1e9999999999999999999999999');
  });
});
