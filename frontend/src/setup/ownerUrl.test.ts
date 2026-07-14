import { describe, expect, it } from 'vitest';
import { canonicalRemote, parseOwner, parseOwnerOnly, repoAlreadyCloned, type ProjectDetail } from './ownerUrl';

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

describe('canonicalRemote', () => {
  it('normalizes https URLs (scheme/host case, trailing .git and slashes)', () => {
    expect(canonicalRemote('HTTPS://GitHub.com/RakuenSoftware/aimee.git')).toBe('https://github.com/rakuensoftware/aimee');
    expect(canonicalRemote('https://github.com/RakuenSoftware/aimee/')).toBe('https://github.com/rakuensoftware/aimee');
  });

  it('maps ssh/scp forms to the same identity as https', () => {
    expect(canonicalRemote('git@github.com:RakuenSoftware/aimee.git')).toBe('https://github.com/rakuensoftware/aimee');
    expect(canonicalRemote('ssh://git@gitea.example.com/org/repo.git')).toBe('https://gitea.example.com/org/repo');
  });

  it('strips userinfo, port, query and fragment', () => {
    expect(canonicalRemote('https://user:tok3n@github.com/org/repo.git?x=1#top')).toBe('https://github.com/org/repo');
    expect(canonicalRemote('https://gitea.example.com:3000/org//repo')).toBe('https://gitea.example.com/org/repo');
  });

  it('keeps path case on self-hosted hosts', () => {
    expect(canonicalRemote('https://gitea.example.com/MyOrg/Repo')).toBe('https://gitea.example.com/MyOrg/Repo');
  });

  it('returns empty for unparseable input', () => {
    expect(canonicalRemote('')).toBe('');
    expect(canonicalRemote('not a url')).toBe('');
    expect(canonicalRemote('github.com/owner/repo')).toBe(''); // no scheme, not scp-like
  });
});

describe('repoAlreadyCloned', () => {
  const repo = { name: 'aimee', clone_url: 'https://github.com/RakuenSoftware/aimee.git' };

  it('matches by canonical remote regardless of the project name/org', () => {
    const details: ProjectDetail[] = [
      { ref: 'acme/renamed', org: 'acme', name: 'renamed', remote: 'https://github.com/rakuensoftware/aimee' },
    ];
    expect(repoAlreadyCloned(repo, 'RakuenSoftware', ['acme/renamed'], details)).toBe(true);
  });

  it('falls back to ref/name only for legacy rows without a remote', () => {
    const details: ProjectDetail[] = [{ ref: 'aimee', org: '', name: 'aimee' }];
    expect(repoAlreadyCloned(repo, 'RakuenSoftware', ['aimee'], details)).toBe(true);
    // A same-named repo elsewhere is NOT masked once the clone has a remote.
    const withRemote: ProjectDetail[] = [
      { ref: 'aimee', org: '', name: 'aimee', remote: 'https://gitea.example.com/other/aimee' },
    ];
    expect(repoAlreadyCloned(repo, 'RakuenSoftware', ['aimee'], withRemote)).toBe(false);
  });

  it('uses the plain projects list when the server sends no details', () => {
    expect(repoAlreadyCloned(repo, 'RakuenSoftware', ['RakuenSoftware/aimee'], [])).toBe(true);
    expect(repoAlreadyCloned(repo, 'RakuenSoftware', ['other'], [])).toBe(false);
  });
});
