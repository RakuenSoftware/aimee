import { describe, it, expect } from 'vitest';
import { parsePs, remoteSetCommand } from './DeployPanel';

/* `docker compose ps --format json` has two wire shapes across versions: a single
 * JSON array (older) and newline-delimited objects (newer). parsePs must handle
 * both, tolerate blank/garbage lines, and normalize Name/Service + State/Status. */
describe('parsePs', () => {
  it('parses a JSON array', () => {
    const s = JSON.stringify([
      { Name: 'aimee-kb-1', State: 'running' },
      { Name: 'postgres-1', State: 'running' },
    ]);
    expect(parsePs(s)).toEqual([
      { name: 'aimee-kb-1', state: 'running' },
      { name: 'postgres-1', state: 'running' },
    ]);
  });

  it('parses newline-delimited objects and skips blank/garbage lines', () => {
    const s = [
      '{"Name":"aimee-kb-1","State":"running"}',
      '',
      'not json',
      '{"Service":"aimee-llm","Status":"Up (healthy)"}',
    ].join('\n');
    expect(parsePs(s)).toEqual([
      { name: 'aimee-kb-1', state: 'running' },
      { name: 'aimee-llm', state: 'Up (healthy)' },
    ]);
  });

  it('returns [] for empty or unrecognized output', () => {
    expect(parsePs('')).toEqual([]);
    expect(parsePs('   ')).toEqual([]);
    expect(parsePs('total garbage, not json at all')).toEqual([]);
  });

  it('drops entries with no resolvable name', () => {
    expect(parsePs('{"State":"running"}')).toEqual([]);
  });
});

describe('remoteSetCommand', () => {
  it('builds the one-command first-user enrollment', () => {
    expect(remoteSetCommand('aimee.example', 8743, 'secret')).toBe(
      'aimee remote set https://aimee.example:8743 secret',
    );
  });

  it('brackets an IPv6 host', () => {
    expect(remoteSetCommand('2001:db8::10', 9443, 'secret')).toBe(
      'aimee remote set https://[2001:db8::10]:9443 secret',
    );
  });
});
