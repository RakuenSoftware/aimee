import { useEffect, useState } from 'react';
import { healthBanner, type HealthBanner as Banner, type HealthSnapshot } from '../setup/health';

/* Full-width strip below the header, shown only when a dependency is actually
 * down. It exists because a degraded instance is otherwise indistinguishable
 * from a healthy one: search just returns nothing, and the user concludes their
 * content isn't there rather than that aimee can't reach its own services.
 *
 * Not dismissible. A dismissed banner would leave the user back in the state
 * this is here to prevent, and it disappears on its own the moment the
 * dependency recovers. All wording/threshold logic lives in the tested
 * setup/health module; this component only polls and renders. */

const POLL_MS = 30_000;

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

export default function HealthBanner() {
  const [banner, setBanner] = useState<Banner | null>(null);

  useEffect(() => {
    let cancelled = false;

    const poll = async () => {
      let snap: HealthSnapshot | null = null;
      try {
        const r = await fetch('/api/ready', { headers: { 'X-CSRF-Token': csrf() } });
        snap = (await r.json()) as HealthSnapshot;
      } catch {
        /* A failed poll is not itself evidence of a degraded server (the browser
           may be offline, or mid-reload). Leave the last known state alone
           rather than flapping the banner on every transient fetch error. */
        return;
      }
      if (!cancelled) setBanner(healthBanner(snap));
    };

    poll();
    const id = setInterval(poll, POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  if (!banner) return null;

  return (
    <div
      role="status"
      aria-live="polite"
      style={{
        flexShrink: 0, background: '#3a2416', borderBottom: '1px solid #6b4423',
        color: '#f0c088', padding: '8px 14px', fontSize: 13, lineHeight: 1.45,
      }}
    >
      <strong style={{ color: '#ffd9a0' }}>{banner.title}</strong>
      <div style={{ color: '#d8b48a', marginTop: 2 }}>{banner.detail}</div>
    </div>
  );
}
