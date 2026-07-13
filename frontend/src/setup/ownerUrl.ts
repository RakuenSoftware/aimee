/* Owner/org URL parsing shared by the wizard's Workspaces step and the Projects
 * page. Pure (no DOM/network), unit-tested. */

export interface OwnerRef {
  host: string;
  owner: string;
}

/** Parse "github.com/RakuenSoftware", "https://github.com/RakuenSoftware/", or
 * "git@host:owner" into {host, owner}. Ignores anything after the owner segment,
 * so a full repo URL also parses (to its owner). Returns null when either part
 * is missing. */
export function parseOwner(input: string): OwnerRef | null {
  let s = input.trim();
  if (!s) return null;
  s = s.replace(/^[a-z]+:\/\//i, ''); // strip scheme
  s = s.replace(/^git@/i, '').replace(':', '/'); // git@host:owner → host/owner
  const parts = s.split('/').filter(Boolean);
  if (parts.length < 2) return null;
  const host = parts[0].toLowerCase();
  const owner = parts[1];
  if (!host.includes('.') || !owner) return null;
  return { host, owner };
}

/** Like parseOwner, but ONLY for an owner/org reference with no repo segment —
 * "github.com/RakuenSoftware" matches, "github.com/RakuenSoftware/aimee" does
 * not. Lets a single URL field distinguish "clone this repo" from "enumerate
 * this owner's repos". */
export function parseOwnerOnly(input: string): OwnerRef | null {
  let s = input.trim();
  if (!s) return null;
  s = s.replace(/^[a-z]+:\/\//i, '');
  s = s.replace(/^git@/i, '').replace(':', '/');
  const parts = s.split('/').filter(Boolean);
  if (parts.length !== 2) return null; // host + owner, nothing after
  return parseOwner(input);
}
