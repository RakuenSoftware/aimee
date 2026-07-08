/* Wizard "dismissed" flag — localStorage, per-browser (MVP; a per-user server
 * store is a follow-up). Guarded for node/SSR and storage-disabled browsers, same
 * as tutorialState. The custom-event name that opens the wizard lives here too so
 * the chip, Settings, and App all agree on one string. */

export const DISMISSED_KEY = 'aimee_setup_dismissed';
export const OPEN_WIZARD_EVENT = 'aimee:open-setup-wizard';
/** Fired after the wizard changes config so the header chip re-computes. */
export const SETUP_UPDATED_EVENT = 'aimee:setup-updated';

export function isDismissed(): boolean {
  try {
    if (typeof localStorage === 'undefined') return false;
    return localStorage.getItem(DISMISSED_KEY) === '1';
  } catch {
    return false;
  }
}

export function setDismissed(dismissed: boolean): void {
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

/** Fire the event that tells the chip to re-read config after a wizard change. */
export function notifySetupUpdated(): void {
  try {
    if (typeof window !== 'undefined') window.dispatchEvent(new CustomEvent(SETUP_UPDATED_EVENT));
  } catch {
    /* ignore */
  }
}
