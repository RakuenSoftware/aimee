import { memo, useMemo, useState } from 'react';
import { tokens, DiffViewer } from '@rakuensoftware/smoothgui';
import { escHtml, renderMd } from './markdown';

interface BootstrapStack {
  name: string;
}

interface BannerProps {
  onGenerate: () => void;
  onDismiss: () => void;
  stacks: BootstrapStack[];
}

export function BootstrapBanner({ onGenerate, onDismiss, stacks }: BannerProps) {
  const stackText =
    stacks.length > 0
      ? 'Detected stack: ' + stacks.map(s => s.name).join(', ') + '.'
      : 'No stack detected.';

  return (
    <div style={{ margin: '8px 16px 0', padding: '10px 14px', background: '#1a2a1a', border: '1px solid #2d4d2d', borderRadius: '6px', color: '#adb', flexShrink: 0 }}>
      <div style={{ fontWeight: 'bold', fontSize: '13px', color: '#8c8' }}>No .aimee-rules file found</div>
      <div style={{ fontSize: '12px', color: '#bbb', margin: '3px 0' }}>{stackText}</div>
      <div style={{ marginTop: '8px', display: 'flex', gap: '8px' }}>
        <button onClick={onGenerate} style={{ padding: '5px 12px', fontSize: '12px', borderRadius: '4px', cursor: 'pointer', border: '1px solid #2d4d2d', background: '#1a3a1a', color: '#8c8' }}>
          Generate .aimee-rules
        </button>
        <button onClick={onDismiss} style={{ padding: '5px 12px', fontSize: '12px', borderRadius: '4px', cursor: 'pointer', border: `1px solid ${tokens.borderMedium}`, background: 'transparent', color: tokens.textSecondary }}>
          Dismiss
        </button>
      </div>
    </div>
  );
}

/* A single chat bubble. Takes the RAW text and renders markdown (assistant) or
 * escapes (user) INTERNALLY, memoized on the text. Combined with React.memo on
 * the component, this means a streaming turn only re-parses the one message whose
 * text is growing — the other (unchanged) messages are skipped entirely, instead
 * of every message being re-parsed on every token. */
export const Message = memo(function Message({ role, text, streaming }: { role: 'user' | 'assistant'; text: string; streaming?: boolean }) {
  // While an assistant bubble is the actively-growing tail, render its RAW text
  // and SKIP the markdown parse: re-parsing the whole growing body on every
  // streamed flush is the dominant streaming CPU cost. Markdown is rendered once,
  // when the bubble stops streaming (the turn ends or another block follows it).
  const html = useMemo(
    () => (streaming ? '' : role === 'user' ? escHtml(text) : renderMd(text)),
    [role, text, streaming],
  );
  const styles: React.CSSProperties = {
    maxWidth: '80%',
    padding: '10px 14px',
    borderRadius: '10px',
    fontSize: '14px',
    lineHeight: 1.5,
    wordWrap: 'break-word',
    overflowWrap: 'break-word',
    wordBreak: 'normal',
    hyphens: 'none',
    alignSelf: role === 'user' ? 'flex-end' : 'flex-start',
    background: role === 'user' ? '#1a3a5c' : tokens.surface,
    color: role === 'user' ? '#cdf' : tokens.text,
    border: role === 'assistant' ? `1px solid ${tokens.border}` : 'none',
    borderBottomRightRadius: role === 'user' ? 2 : 10,
    borderBottomLeftRadius: role === 'assistant' ? 2 : 10,
  };
  if (streaming) return <div style={{ ...styles, whiteSpace: 'pre-wrap' }}>{text}</div>;
  return <div style={styles} dangerouslySetInnerHTML={{ __html: html }} />;
});

export const ThinkingBlock = memo(function ThinkingBlock({ text }: { text: string }) {
  return (
    <details style={{ margin: '4px 0', padding: '8px 10px', background: tokens.surfaceAlt, borderLeft: `3px solid ${tokens.borderMedium}`, borderRadius: '4px', fontSize: '12px', color: tokens.textFaint, alignSelf: 'flex-start', maxWidth: '80%' }}>
      <summary style={{ cursor: 'pointer', color: tokens.textPale, fontStyle: 'italic' }}>Thinking…</summary>
      <pre style={{ marginTop: '6px', fontSize: '11px', color: tokens.textHint, maxHeight: '200px', overflowY: 'auto', whiteSpace: 'pre-wrap' }}>{text}</pre>
    </details>
  );
});

export const ToolBlock = memo(function ToolBlock({ name, args, result }: { name: string; args: string; result?: string }) {
  const pending = result === undefined;
  return (
    <details style={{ margin: '4px 0', padding: '8px 10px', background: tokens.surfaceAlt, borderLeft: `3px solid ${tokens.primary}`, borderRadius: '4px', fontSize: '12px', color: tokens.textFaint, alignSelf: 'flex-start', maxWidth: '80%' }}>
      <summary style={{ cursor: 'pointer', color: tokens.primary, fontSize: '12px', display: 'flex', alignItems: 'center', gap: '6px' }}>
        {pending ? <span style={{ color: tokens.textHint, fontStyle: 'italic' }}>Running: {name}</span> : <span>Tool: {name}</span>}
      </summary>
      {args && <pre style={{ marginTop: '6px', fontSize: '11px', color: tokens.textPale, maxHeight: '150px', overflowY: 'auto', whiteSpace: 'pre-wrap' }}>{args}</pre>}
      {result && (
        <details style={{ marginTop: '4px' }}>
          <summary style={{ cursor: 'pointer', fontSize: '11px', color: tokens.textHint }}>Output</summary>
          <pre style={{ marginTop: '4px', fontSize: '11px', color: tokens.textPale, maxHeight: '200px', overflowY: 'auto', whiteSpace: 'pre-wrap' }}>{result}</pre>
        </details>
      )}
    </details>
  );
});

export const TurnSummaryCard = memo(function TurnSummaryCard({ text }: { text: string }) {
  return (
    <details style={{ margin: '2px 0', padding: '6px 10px', background: tokens.surfaceAlt, borderLeft: `3px solid ${tokens.borderMedium}`, borderRadius: '4px', fontSize: '12px', color: tokens.textFaint, alignSelf: 'flex-start', maxWidth: '80%' }}>
      <summary style={{ cursor: 'pointer', color: tokens.textSecondary, fontStyle: 'italic', userSelect: 'none' }}>Turn summary</summary>
      <div style={{ marginTop: '6px', fontSize: '12px', color: tokens.textPale, lineHeight: 1.5 }}>{text}</div>
    </details>
  );
});

export const DiffBlock = memo(function DiffBlock({ path, diff }: { path?: string; diff: string }) {
  return (
    <details style={{ margin: '4px 0', padding: '8px 10px', background: '#0d1117', borderLeft: '3px solid #444', borderRadius: '4px', fontSize: '12px', alignSelf: 'flex-start', maxWidth: '90%' }}>
      <summary style={{ cursor: 'pointer', color: '#8bf', fontSize: '12px', fontFamily: 'monospace' }}>{path ? `diff: ${path}` : 'diff'}</summary>
      <DiffViewer diff={diff} />
    </details>
  );
});

export function RewindMarker({ snapshotId, sid }: { snapshotId: number; sid: string }) {
  const [status, setStatus] = useState<'idle' | 'pending' | 'done' | 'error'>('idle');

  async function handleRewind() {
    setStatus('pending');
    try {
      const resp = await fetch('/api/chat/rewind', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Session-Id': sid },
        body: JSON.stringify({ snapshot_id: snapshotId }),
        credentials: 'include',
      });
      setStatus(resp.ok ? 'done' : 'error');
    } catch {
      setStatus('error');
    }
  }

  return (
    <div style={{ alignSelf: 'flex-start', display: 'flex', alignItems: 'center', gap: '8px', padding: '3px 10px', margin: '2px 0', background: tokens.surfaceAlt, border: `1px solid ${tokens.borderLight}`, borderRadius: '8px', fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui' }}>
      <span>Snapshot #{snapshotId}</span>
      {status === 'idle' && (
        <button onClick={handleRewind} style={{ padding: '2px 8px', fontSize: '11px', fontFamily: 'system-ui', background: 'transparent', color: tokens.primary, border: `1px solid ${tokens.primary}`, borderRadius: '6px', cursor: 'pointer' }}>
          Rewind to here
        </button>
      )}
      {status === 'pending' && <span style={{ color: tokens.textHint }}>Rewinding…</span>}
      {status === 'done' && <span style={{ color: '#4ec94e' }}>Restored - conversation cleared</span>}
      {status === 'error' && <span style={{ color: '#f87171' }}>Rewind failed</span>}
    </div>
  );
}
