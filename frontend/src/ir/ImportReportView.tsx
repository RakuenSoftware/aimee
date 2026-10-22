import { useEffect, useState } from 'react';
import type { ImportObservation } from './importReport';

export default function ImportReportView({ observation }: { observation: ImportObservation }) {
  const { report, chunks } = observation;
  const [verified, setVerified] = useState<'pending' | 'yes' | 'no'>('pending');
  const [open, setOpen] = useState(false);
  useEffect(() => {
    if (report.outcome !== 'archived') return;
    let disposed = false;
    setVerified('pending');
    void (async () => {
      try {
        if (!chunks || chunks.length !== report.archive_chunks || !globalThis.crypto?.subtle) throw new Error('Archive verification unavailable');
        const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(chunks.join('')));
        const actual = Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('');
        if (!disposed) setVerified(actual === report.source_sha256 ? 'yes' : 'no');
      } catch { if (!disposed) setVerified('no'); }
    })();
    return () => { disposed = true; };
  }, [report.outcome, report.source_sha256, report.archive_chunks, chunks]);
  if (report.outcome === 'archived') {
    if (verified !== 'yes') return <p role="note" aria-live="off">{verified === 'pending' ? 'Verifying archived legacy terminal text…' :
      'Archived legacy terminal text could not be verified. It was not added to model history.'}</p>;
    return <section role="note" aria-live="off">
      <p>Legacy terminal text was archived outside model history. Original message roles and tool structure could not be recovered.</p>
      <details onToggle={event => setOpen(event.currentTarget.open)}><summary>View archived terminal text</summary>
        {open && <pre style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere', maxHeight: 320, overflowY: 'auto' }}>{chunks?.join('')}</pre>}
      </details>
    </section>;
  }
  return <section role="note" aria-live="off">
    <p>{report.outcome === 'refused' ? 'History import was refused; it was not added to model history.' :
      `History imported ${report.outcome === 'exact' ? 'exactly' : 'after normalization'} (${report.imported_messages} ${report.imported_messages === 1 ? 'message' : 'messages'}).`}</p>
    {report.removed_thinking_bindings > 0 && <p>{report.removed_thinking_bindings} reasoning replay {report.removed_thinking_bindings === 1 ? 'binding was' : 'bindings were'} removed.</p>}
    <p>{report.tool_structure === 'preserved' ? 'Tool structure was preserved.' : 'Tool structure could not be recovered.'}</p>
  </section>;
}
