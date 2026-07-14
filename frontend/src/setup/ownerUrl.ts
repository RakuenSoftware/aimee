/* Owner/org URL parsing and project-identity helpers shared by the wizard's
 * Workspaces step and the Projects page. Pure (no DOM/network), unit-tested. */

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

/** One row of GET /api/git/projects `details` — a cloned project's identity.
 * `remote` is the credential-free canonical remote; absent for legacy clones
 * made before the sidecar existed. */
export interface ProjectDetail {
  ref: string;
  org: string;
  name: string;
  remote?: string;
}

/** GET /api/git/projects response (the fields the GUI consumes). `projects`
 * holds project REFS — "org/name" or a flat "name". */
export interface GitProjectsResponse {
  projects?: string[];
  details?: ProjectDetail[];
  root?: string;
  error?: string;
}

/* Knowledge-indexing annotations a clone response (or a clone-org per-repo
 * result row) may carry: `kb_indexed:false` means the clone succeeded but the
 * code scan did not run — `kb_reason` says why. A single clone may also carry
 * `org_note` (multi-segment owner cloned flat; pass an explicit org). */
export interface CloneKbAnnotations {
  org_note?: string;
  kb_indexed?: boolean;
  kb_reason?: string;
}

/** POST /api/git/projects/delete response. 200 carries `kb_status`
 * ("purged" | "retained" | "forced") + `purge_id`; 503 (kb unavailable —
 * retry or force) and 400/404 carry `error`. `kb` is the per-store detail. */
export interface ProjectDeleteResponse {
  ok?: boolean;
  ref?: string;
  kb_status?: 'purged' | 'retained' | 'forced';
  purge_id?: string;
  kb?: Record<string, unknown>;
  error?: string;
}

/** One org heading on the Projects page: the org name ('' = ungrouped legacy
 * flat clones) and its project refs in server order. */
export interface OrgGroup {
  org: string;
  refs: string[];
}

/** Group project refs under their orgs for display. The org comes from the
 * details row when the server sent one, else from the ref itself ("org/name" →
 * "org", flat → ''). Named orgs sort alphabetically; the ungrouped bucket
 * (legacy flat clones) comes last. Refs keep server order within a group. */
export function groupProjectsByOrg(projects: string[], details: ProjectDetail[]): OrgGroup[] {
  const byRef = new Map(details.map(d => [d.ref, d]));
  const groups = new Map<string, string[]>();
  for (const ref of projects) {
    const d = byRef.get(ref);
    const slash = ref.indexOf('/');
    const org = d ? d.org : slash > 0 ? ref.slice(0, slash) : '';
    const bucket = groups.get(org);
    if (bucket) bucket.push(ref);
    else groups.set(org, [ref]);
  }
  return [...groups.entries()]
    .sort(([a], [b]) => (a === '' ? 1 : b === '' ? -1 : a.localeCompare(b)))
    .map(([org, refs]) => ({ org, refs }));
}

// Hosts whose repo paths are case-insensitive (mirrors the server's
// util_url_host_is_case_insensitive).
const CI_HOSTS = ['github.com', 'gitlab.com', 'bitbucket.org'];

/** Canonicalize a git remote URL to the server's credential-free form
 * (git_project_canonical_remote): scheme forced to https, host lowercased and
 * port-stripped, userinfo/query/fragment dropped, '//' runs collapsed, path
 * lowercased on case-insensitive hosts, trailing slashes and a trailing
 * ".git" stripped. Returns '' when the URL cannot be parsed. */
export function canonicalRemote(url: string): string {
  let s = (url || '').trim();
  if (!s || /[\s]/.test(s)) return '';
  s = s.replace(/[?#].*$/, ''); // query/fragment never identify a repo
  let host: string;
  let path: string;
  const proto = s.match(/^(https?|ssh|git):\/\//i);
  if (proto) {
    const rest = s.slice(proto[0].length);
    const slash = rest.indexOf('/');
    if (slash <= 0) return ''; // must have a host before the path
    let auth = rest.slice(0, slash);
    path = rest.slice(slash + 1);
    const at = auth.indexOf('@');
    if (at >= 0) auth = auth.slice(at + 1); // strip userinfo — never creds
    host = auth;
  } else {
    // scp-like: user@host:path (no scheme)
    const m = s.match(/^[^@]+@([^:]+):(.+)$/);
    if (!m) return '';
    host = m[1];
    path = m[2];
  }
  host = host.toLowerCase().replace(/:\d*$/, '');
  if (!host) return '';
  if (CI_HOSTS.some(h => host === h || host.endsWith('.' + h))) path = path.toLowerCase();
  path = path.replace(/\/{2,}/g, '/').replace(/^\/+/, '').replace(/\/+$/, '');
  if (path.endsWith('.git')) path = path.slice(0, -4);
  path = path.replace(/\/+$/, '');
  if (!path) return '';
  return `https://${host}/${path}`;
}

/** True when an enumerated repo is already cloned into the workspace. Matches
 * by canonical remote (robust to the project's chosen name/org); rows without
 * a recorded remote (legacy clones) — or an older server sending no details at
 * all — fall back to the ref/name comparison. */
export function repoAlreadyCloned(
  repo: { name: string; clone_url: string },
  owner: string,
  projects: string[],
  details: ProjectDetail[],
): boolean {
  const canon = canonicalRemote(repo.clone_url);
  if (canon && details.some(d => d.remote && canonicalRemote(d.remote) === canon)) return true;
  const legacyRefs = details.length > 0
    ? details.filter(d => !d.remote).map(d => d.ref)
    : projects;
  return legacyRefs.includes(`${owner}/${repo.name}`) || legacyRefs.includes(repo.name);
}
