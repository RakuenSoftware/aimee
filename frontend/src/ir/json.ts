/** Canonical opaque JSON must retain decimal values outside JavaScript's
 * roundtrip range. Known numeric contract fields still require plain numbers. */
const numberToken = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;
function decimalKey(token: string): string | undefined {
  const parts = /^(-?)([0-9]+)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?$/.exec(token);
  if (!parts) return undefined;
  const digits = (parts[2] + (parts[3] ?? '')).replace(/^0+/, '');
  if (!digits) return '0';
  if ((parts[4] ?? '').length > 7) return undefined;
  const significant = digits.replace(/0+$/, '');
  const exponent = Number(parts[4] ?? '0') - (parts[3]?.length ?? 0) + digits.length - significant.length;
  return `${parts[1]}${significant}e${exponent}`;
}
export class ExactJSONNumber {
  readonly token: string;
  constructor(token: string) {
    numberToken.lastIndex = 0;
    if (numberToken.exec(token)?.[0] !== token) throw new SyntaxError('Invalid decimal token');
    this.token = token;
    Object.freeze(this);
  }
  toJSON(): never { throw new TypeError('Exact JSON numbers require stringifyJSON'); }
}
function decimal(token: string): number | ExactJSONNumber {
  const value = Number(token);
  if (Number.isFinite(value) && !Object.is(value, -0) &&
      (!Number.isInteger(value) || Number.isSafeInteger(value)) &&
      decimalKey(token) !== undefined && decimalKey(token) === decimalKey(JSON.stringify(value))) return value;
  return new ExactJSONNumber(token);
}
export function parseJSON(source: string): unknown {
  let offset = 0;
  const whitespace = () => { while (/[\t\r\n ]/.test(source[offset] ?? '') && offset < source.length) offset++; };
  const fail = (): never => { throw new SyntaxError('Invalid canonical JSON'); };
  function string(): string {
    const start = offset++;
    while (offset < source.length) {
      const char = source[offset++];
      if (char === '"') return JSON.parse(source.slice(start, offset)) as string;
      if (char === '\\') offset++; // Native string decoding validates escapes and controls.
    }
    return fail();
  }
  function value(depth: number): unknown {
    if (depth > 512) return fail();
    whitespace();
    const char = source[offset];
    if (char === '"') return string();
    if (char === '[' || char === '{') {
      offset++;
      const array = char === '[';
      const result: unknown[] | Record<string, unknown> = array ? [] : {};
      const end = array ? ']' : '}';
      whitespace();
      if (source[offset] === end) { offset++; return result; }
      for (;;) {
        whitespace();
        let key = '';
        if (!array) {
          if (source[offset] !== '"') return fail();
          key = string(); whitespace();
          if (Object.prototype.hasOwnProperty.call(result, key)) return fail();
          if (source[offset++] !== ':') return fail();
        }
        const child = value(depth + 1);
        if (array) (result as unknown[]).push(child);
        else Object.defineProperty(result, key, { value: child, enumerable: true, writable: true, configurable: true });
        whitespace();
        const delimiter = source[offset++];
        if (delimiter === end) return result;
        if (delimiter !== ',') return fail();
      }
    }
    for (const [literal, primitive] of [['true', true], ['false', false], ['null', null]] as const) {
      if (source.startsWith(literal, offset)) { offset += literal.length; return primitive; }
    }
    numberToken.lastIndex = offset;
    const match = numberToken.exec(source);
    if (!match) return fail();
    offset = numberToken.lastIndex;
    return decimal(match[0]);
  }
  const result = value(0);
  whitespace();
  if (offset !== source.length) return fail();
  return result;
}
export function stringifyJSON(value: unknown, space = 0): string | undefined {
  const indent = ' '.repeat(Math.min(10, Math.max(0, Math.trunc(space))));
  const ancestors = new Set<object>();
  function encode(current: unknown, depth: number): string | undefined {
    if (current instanceof ExactJSONNumber) return current.token;
    if (current === null || typeof current !== 'object') return JSON.stringify(current);
    if (ancestors.has(current) || depth > 512) throw new TypeError('Invalid JSON tree');
    ancestors.add(current);
    const array = Array.isArray(current);
    const entries = array ? Array.from(current, child => encode(child, depth + 1) ?? 'null') :
      Object.entries(current).flatMap(([key, child]) => {
        const encoded = encode(child, depth + 1);
        return encoded === undefined ? [] : [`${JSON.stringify(key)}:${indent ? ' ' : ''}${encoded}`];
      });
    ancestors.delete(current);
    const [open, close] = array ? ['[', ']'] : ['{', '}'];
    if (!entries.length) return `${open}${close}`;
    return indent ? `${open}\n${indent.repeat(depth + 1)}${entries.join(`,\n${indent.repeat(depth + 1)}`)}\n${indent.repeat(depth)}${close}` :
      `${open}${entries.join(',')}${close}`;
  }
  return encode(value, 0);
}
