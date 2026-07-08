import { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { tutorialFor } from '../help/tutorials';
import { hasSeen, markSeen } from '../help/tutorialState';

/* Per-tab tutorial affordance. Mounted once in App inside <main>, keyed on the
 * active route. On the first visit to a tab that has a tutorial it auto-opens a
 * dismissible card; after dismissal it collapses to a small "?" button in the
 * top-right that re-opens it. A route with no tutorial renders nothing.
 *
 * It is deliberately NON-MODAL: there is no full-area backdrop, so the rest of
 * the page stays fully interactive while the card is open (the card is a corner
 * panel, not a blocking overlay). All persistence + content lookups live in the
 * tested help/ modules; this component is just presentation. */

export default function TabTutorial({ route }: { route: string }) {
  const navigate = useNavigate();
  const tut = tutorialFor(route);
  const [open, setOpen] = useState(false);

  // On each route change, auto-open only if this tab has a tutorial the operator
  // hasn't dismissed yet.
  useEffect(() => {
    if (tut && !hasSeen(route)) setOpen(true);
    else setOpen(false);
  }, [route, tut]);

  // Escape closes the card (counts as seen) when it is open.
  useEffect(() => {
    if (!open) return;
    function onKey(e: KeyboardEvent) {
      if (e.key === 'Escape') { markSeen(route); setOpen(false); }
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, route]);

  if (!tut) return null;

  function dismiss() {
    markSeen(route);
    setOpen(false);
  }

  // Collapsed state: just the "?" re-opener in the corner.
  if (!open) {
    return (
      <button
        aria-label="Show tab help"
        title="What is this tab?"
        onClick={() => setOpen(true)}
        style={{
          position: 'absolute', top: 10, right: 12, zIndex: 20,
          width: 26, height: 26, borderRadius: '50%', cursor: 'pointer',
          border: '1px solid #ccd', background: '#fff', color: '#68a',
          fontSize: 14, fontWeight: 700, lineHeight: 1,
        }}
      >?</button>
    );
  }

  // Open state: a non-modal card anchored top-right. No backdrop — the page
  // underneath stays clickable, so the tutorial never blocks work.
  return (
    <div
      role="dialog"
      aria-label={`${tut.title} tab help`}
      style={{
        position: 'absolute', top: 10, right: 12, zIndex: 25,
        maxWidth: 380, width: 'min(380px, calc(100% - 24px))',
        background: '#fff', borderRadius: 10, border: '1px solid #dde',
        boxShadow: '0 8px 30px rgba(0,0,0,0.18)', padding: '16px 18px',
        fontFamily: 'system-ui', color: '#334',
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 }}>
        <span style={{ fontSize: 15, fontWeight: 700, color: '#223' }}>{tut.title}</span>
        <button
          aria-label="Close tab help"
          title="Close"
          onClick={dismiss}
          style={{ background: 'none', border: 'none', color: '#9aa', cursor: 'pointer', fontSize: 18, lineHeight: 1, padding: 0 }}
        >×</button>
      </div>
      {tut.body.map((line, i) => (
        <p key={i} style={{ fontSize: 13, lineHeight: 1.5, margin: '0 0 7px' }}>{line}</p>
      ))}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginTop: 12 }}>
        {tut.seeAlso ? (
          <button
            onClick={() => { dismiss(); navigate(tut.seeAlso!); }}
            style={{ background: 'none', border: 'none', color: '#68a', cursor: 'pointer', fontSize: 12.5, padding: 0 }}
          >
            See also: {tutorialFor(tut.seeAlso)?.title ?? tut.seeAlso} →
          </button>
        ) : <span />}
        <button
          onClick={dismiss}
          style={{
            padding: '5px 14px', borderRadius: 6, border: '1px solid #ccd',
            background: '#f4f6fb', color: '#446', cursor: 'pointer', fontSize: 12.5, fontWeight: 600,
          }}
        >Got it</button>
      </div>
    </div>
  );
}
