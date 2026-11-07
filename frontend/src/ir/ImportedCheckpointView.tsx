import { useEffect, useState, type ReactNode } from 'react';
import { parseJSON } from './json';
import { validateHistoryMessage, type Message } from './session';
import type { ImportObservation } from './importReport';

/** Read-only view of the acknowledged import base. It never enters live turn state. */
export default function ImportedCheckpointView({ observation, renderMessages }: {
  observation: ImportObservation; renderMessages: (messages: Message[]) => ReactNode;
}) {
  const checkpoint = observation.checkpoint;
  const [verified, setVerified] = useState<{ checkpoint: typeof checkpoint; messages?: Message[] }>({ checkpoint: undefined });
  useEffect(() => {
    if (!checkpoint?.committed) return;
    let disposed = false;
    void (async () => {
      try {
        const messages: Message[] = [];
        for (const entry of checkpoint.entries) {
          let message: unknown;
          if ('message' in entry) message = entry.message;
          else {
            if (entry.parts.length !== entry.count || !globalThis.crypto?.subtle) throw new Error('Incomplete imported message');
            const raw = entry.parts.join('');
            const hash = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(raw));
            const digest = Array.from(new Uint8Array(hash), byte => byte.toString(16).padStart(2, '0')).join('');
            if (digest !== entry.sha256) throw new Error('Imported message digest mismatch');
            message = parseJSON(raw);
          }
          validateHistoryMessage(message);
          messages.push(message);
        }
        if (messages.length !== observation.report.imported_messages) throw new Error('Imported message count mismatch');
        if (!disposed) setVerified({ checkpoint, messages });
      } catch { if (!disposed) setVerified({ checkpoint }); }
    })();
    return () => { disposed = true; };
  }, [checkpoint, observation.report.imported_messages]);
  if (!checkpoint?.committed) return null;
  if (verified.checkpoint !== checkpoint) return <p role="note" aria-live="off">Verifying imported conversation…</p>;
  if (!verified.messages) return <p role="note" aria-live="off">Imported conversation could not be verified and is hidden.</p>;
  return <section aria-label="Imported historical conversation">{renderMessages(verified.messages)}</section>;
}
