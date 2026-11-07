/* Setup dismissal is a browser presentation preference, never account
 * readiness. Pending accounts use an authenticated per-install/account scope;
 * the legacy Finish flag remains compatible only after account setup. */

export const DISMISSED_KEY = 'aimee_setup_dismissed';
export const OPEN_WIZARD_EVENT = 'aimee:open-setup-wizard';
/** Fired after setup config or the cloned-project inventory changes. */
export const SETUP_UPDATED_EVENT = 'aimee:setup-updated';

let pendingDismissal = false;
let memoryScope: string | undefined;
const scopedKey = (scope: string) => `${DISMISSED_KEY}:scope:${scope}`;
const validScope = (scope: unknown): scope is string => typeof scope === 'string' && /^[a-f0-9]{64}$/.test(scope);

/** An explicit close is a presentation preference, never account completion.
 * Until authenticated status arrives, retain the intent only in this document. */
export function rememberSetupDismissal(scope?: string): void {
  if (!validScope(scope)) { pendingDismissal = true; return; }
  pendingDismissal = false;
  memoryScope = scope;
  try { if (typeof localStorage !== 'undefined') localStorage.setItem(scopedKey(scope), '1'); } catch { /* Memory still honors this close. */ }
}

export function resolveSetupDismissalScope(scope?: string): void {
  if (pendingDismissal && validScope(scope)) rememberSetupDismissal(scope);
}

export function isSetupDismissed(scope?: string): boolean {
  if (pendingDismissal) return true;
  if (!validScope(scope)) return false;
  if (memoryScope === scope) return true;
  try { return typeof localStorage !== 'undefined' && localStorage.getItem(scopedKey(scope)) === '1'; } catch { return false; }
}

export function isDismissed(): boolean {
  try {
    if (typeof localStorage === 'undefined') return false;
    return localStorage.getItem(DISMISSED_KEY) === '1';
  } catch {
    return false;
  }
}

export function setDismissed(dismissed: boolean): void {
  if (!dismissed) { pendingDismissal = false; memoryScope = undefined; }
  try {
    if (typeof localStorage === 'undefined') return;
    if (dismissed) localStorage.setItem(DISMISSED_KEY, '1');
    else localStorage.removeItem(DISMISSED_KEY);
  } catch {
    /* ignore */
  }
}

/** Fire the event that opens the wizard (chip click / "Re-run setup"). */
export function requestOpenWizard(): void {
  try {
    if (typeof window !== 'undefined') window.dispatchEvent(new CustomEvent(OPEN_WIZARD_EVENT));
  } catch {
    /* ignore */
  }
}

/** Fire the event that tells the chip to re-read setup state after a mutation. */
export function notifySetupUpdated(): void {
  try {
    if (typeof window !== 'undefined') window.dispatchEvent(new CustomEvent(SETUP_UPDATED_EVENT));
  } catch {
    /* ignore */
  }
}
