/* "Seen" state for per-tab tutorials. The auto-open-on-first-visit behaviour needs
 * to remember which tabs the operator has already dismissed. For the MVP this is
 * per-browser localStorage (the same pattern App/LogoutButton use for chat tabs).
 *
 * The parse/merge logic is split from the storage side-effects so it can be unit
 * tested under vitest's `node` environment, where `localStorage` does not exist.
 * The storage wrappers guard `typeof localStorage` and swallow quota/security
 * errors, so a private-mode or storage-disabled browser degrades to "always show"
 * rather than throwing. */

export const SEEN_KEY = 'aimee_tutorial_seen';

/** Parse a stored blob into a clean list of route strings. Tolerates null,
 * malformed JSON, and non-array/non-string junk — always returns a string[]. */
export function parseSeen(raw: string | null): string[] {
  if (!raw) return [];
  try {
    const v = JSON.parse(raw);
    if (!Array.isArray(v)) return [];
    return v.filter((x): x is string => typeof x === 'string');
  } catch {
    return [];
  }
}

/** Return a new list with `route` present (idempotent, order-stable). */
export function withSeen(list: string[], route: string): string[] {
  return list.includes(route) ? list : [...list, route];
}

// ── localStorage-backed wrappers (side-effecting; guarded for node/SSR) ──────

function readRaw(): string | null {
  try {
    if (typeof localStorage === 'undefined') return null;
    return localStorage.getItem(SEEN_KEY);
  } catch {
    return null;
  }
}

function writeList(list: string[]): void {
  try {
    if (typeof localStorage === 'undefined') return;
    localStorage.setItem(SEEN_KEY, JSON.stringify(list));
  } catch {
    /* storage disabled/full — degrade to always-show */
  }
}

export function hasSeen(route: string): boolean {
  return parseSeen(readRaw()).includes(route);
}

export function markSeen(route: string): void {
  writeList(withSeen(parseSeen(readRaw()), route));
}

/** Forget all seen tabs — the "Replay tab tutorials" action. */
export function resetAll(): void {
  try {
    if (typeof localStorage === 'undefined') return;
    localStorage.removeItem(SEEN_KEY);
  } catch {
    /* ignore */
  }
}
