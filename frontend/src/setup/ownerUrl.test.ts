import { describe, expect, it } from 'vitest';
import { parseOwner, parseOwnerOnly } from './ownerUrl';

describe('parseOwner', () => {
  it('parses the plain owner form', () => {
    expect(parseOwner('github.com/RakuenSoftware')).toEqual({ host: 'github.com', owner: 'RakuenSoftware' });
  });

  it('strips scheme and trailing slash', () => {
    expect(parseOwner('https://github.com/RakuenSoftware/')).toEqual({ host: 'github.com', owner: 'RakuenSoftware' });
  });

  it('parses the ssh form', () => {
    expect(parseOwner('git@gitea.example.com:MyOrg')).toEqual({ host: 'gitea.example.com', owner: 'MyOrg' });
  });

  it('a full repo URL parses to its owner', () => {
    expect(parseOwner('https://github.com/RakuenSoftware/aimee.git')).toEqual({ host: 'github.com', owner: 'RakuenSoftware' });
  });

  it('rejects hostless or ownerless input', () => {
    expect(parseOwner('RakuenSoftware')).toBeNull();
    expect(parseOwner('github.com')).toBeNull();
    expect(parseOwner('localhost/owner')).toBeNull(); // host needs a dot
    expect(parseOwner('')).toBeNull();
  });
});

describe('parseOwnerOnly', () => {
  it('accepts an owner reference with no repo segment', () => {
    expect(parseOwnerOnly('github.com/RakuenSoftware')).toEqual({ host: 'github.com', owner: 'RakuenSoftware' });
    expect(parseOwnerOnly('https://github.com/RakuenSoftware/')).toEqual({ host: 'github.com', owner: 'RakuenSoftware' });
    expect(parseOwnerOnly('git@github.com:RakuenSoftware')).toEqual({ host: 'github.com', owner: 'RakuenSoftware' });
  });

  it('rejects a full repo URL — that is a single clone, not an enumeration', () => {
    expect(parseOwnerOnly('github.com/RakuenSoftware/aimee')).toBeNull();
    expect(parseOwnerOnly('https://github.com/RakuenSoftware/aimee.git')).toBeNull();
    expect(parseOwnerOnly('git@github.com:RakuenSoftware/aimee.git')).toBeNull();
  });

  it('rejects non-owner input', () => {
    expect(parseOwnerOnly('RakuenSoftware')).toBeNull();
    expect(parseOwnerOnly('')).toBeNull();
  });
});
