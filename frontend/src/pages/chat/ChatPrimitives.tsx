import { memo, useMemo, useState } from 'react';
import { Button, tokens, DiffViewer } from '@rakuensoftware/smoothgui';
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
  return (
    <div className="chat-bootstrap-banner" role="region" aria-label="Project rules notice">
      <div className="chat-bootstrap-copy">
        <strong>.aimee-rules not found</strong>
        {stacks.length > 0 && <details className="chat-bootstrap-stack">
          <summary>Detected stack</summary>
          <span>{stacks.map(s => s.name).join(', ')}</span>
        </details>}
      </div>
      <div className="chat-bootstrap-actions">
        <Button variant="primary" size="sm" aria-label="Generate .aimee-rules" onClick={onGenerate}>Generate</Button>
        <Button variant="ghost" size="sm" aria-label="Dismiss" title="Dismiss project rules notice" onClick={onDismiss}>×</Button>
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
    background: role === 'user' ? 'var(--sg-info-bg)' : tokens.surface,
    color: tokens.text,
    border: role === 'assistant' ? `1px solid ${tokens.border}` : 'none',
    borderBottomRightRadius: role === 'user' ? 2 : 10,
    borderBottomLeftRadius: role === 'assistant' ? 2 : 10,
  };
  if (streaming) return <div style={{ ...styles, whiteSpace: 'pre-wrap' }}>{text}</div>;
  return <div style={styles} dangerouslySetInnerHTML={{ __html: html }} />;
});

export const ThinkingBlock = memo(function ThinkingBlock({ text }: { text: string }) {
  return (
    <details style={{ margin: '4px 0', padding: '8px 10px', background: tokens.surfaceAlt, borderLeft: `3px solid ${tokens.borderMedium}`, borderRadius: '4px', fontSize: '12px', color: tokens.textSecondary, alignSelf: 'flex-start', maxWidth: '80%' }}>
      <summary style={{ cursor: 'pointer', color: tokens.text, fontStyle: 'italic' }}>Reasoning</summary>
      <pre style={{ marginTop: '6px', fontSize: '11px', color: tokens.textSecondary, maxHeight: '200px', overflowY: 'auto', whiteSpace: 'pre-wrap' }}>{text}</pre>
    </details>
  );
});

export const ToolBlock = memo(function ToolBlock({ name, args, result, unknownOutcome }: { name: string; args: string; result?: string; unknownOutcome?: string }) {
  const pending = result === undefined && !unknownOutcome;
  return (
    <details style={{ margin: '4px 0', padding: '8px 10px', background: unknownOutcome ? 'var(--sg-warning-bg)' : tokens.surfaceAlt, borderLeft: `3px solid ${unknownOutcome ? 'var(--sg-warning-border)' : tokens.primary}`, borderRadius: '4px', fontSize: '12px', color: tokens.textSecondary, alignSelf: 'flex-start', maxWidth: '80%' }}>
      <summary style={{ cursor: 'pointer', color: tokens.text, fontSize: '12px', display: 'flex', alignItems: 'center', gap: '6px' }}>
        {pending ? <span style={{ color: tokens.textSecondary, fontStyle: 'italic' }}>Running: {name}</span> : <span>Tool: {name}{unknownOutcome ? ' — outcome unknown' : ''}</span>}
      </summary>
      {args && <pre style={{ marginTop: '6px', fontSize: '11px', color: tokens.text, maxHeight: '150px', overflowY: 'auto', whiteSpace: 'pre-wrap' }}>{args}</pre>}
      {unknownOutcome && <p role="note" aria-live="off" style={{ marginTop: '6px', color: tokens.text }}>{unknownOutcome}</p>}
      {result && (
        <details style={{ marginTop: '4px' }}>
          <summary style={{ cursor: 'pointer', fontSize: '11px', color: tokens.textSecondary }}>Output</summary>
          <pre style={{ marginTop: '4px', fontSize: '11px', color: tokens.text, maxHeight: '200px', overflowY: 'auto', whiteSpace: 'pre-wrap' }}>{result}</pre>
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
    <details style={{ margin: '4px 0', padding: '8px 10px', background: 'var(--sg-dark-surface)', borderLeft: '3px solid var(--sg-text-muted)', borderRadius: '4px', fontSize: '12px', alignSelf: 'flex-start', maxWidth: '90%' }}>
      <summary style={{ cursor: 'pointer', color: 'var(--sg-primary)', fontSize: '12px', fontFamily: 'monospace' }}>{path ? `diff: ${path}` : 'diff'}</summary>
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
      {status === 'done' && <span style={{ color: 'var(--sg-success)' }}>Restored - conversation cleared</span>}
      {status === 'error' && <span style={{ color: 'var(--sg-danger-light)' }}>Rewind failed</span>}
    </div>
  );
}
