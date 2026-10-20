import { parseJSON, stringifyJSON } from './json';
import { useEffect, useRef, useState } from 'react';
import { Message, ThinkingBlock, ToolBlock } from '../pages/chat/ChatPrimitives';
import { emptySession, reduceSession, toolResults, toolBlockKey } from './session';
import type { ChildExecution, Envelope, SessionState } from './session';
import ControlCard, { type ReplyControl } from './ControlCard';
const pretty = (value: unknown) => typeof value === 'string' ? value : stringifyJSON(value, 2) ?? '';
function mediaURL(ref: string | undefined): string | undefined {
  if (ref && (/^https?:\/\//i.test(ref) || /^data:image\/(png|jpeg|webp|gif);base64,/i.test(ref) || ref.startsWith('/api/artifacts/'))) return ref;
  return undefined;
}
function ChildExecutionView({ reference, ancestors }: { reference: ChildExecution; ancestors: string[] }) {
  const [expanded, setExpanded] = useState(false);
  const current = useRef(emptySession(reference.session_id));
  const [state, setState] = useState(current.current);
  const [error, setError] = useState('');
  const cyclic = ancestors.includes(reference.session_id);
  useEffect(() => {
    if (!expanded || cyclic) return;
    setError('');
    let disposed = false;
    const stream = new EventSource(`/api/chat/session-events?format=ir&sid=${encodeURIComponent(reference.session_id)}&cursor=${current.current.sequence}`);
    stream.addEventListener('ir', (message) => {
      if (disposed) return;
      try {
        const envelope = parseJSON((message as MessageEvent).data) as Envelope;
        const next = reduceSession(current.current, envelope);
        current.current = next;
        setState(next);
        const status = next.executions[reference.turn_id];
        if (status && status !== 'running') { disposed = true; stream.close(); }
      } catch {
        disposed = true;
        setError('Delegate history could not be read. Close and reopen to retry.');
        stream.close();
      }
    });
    stream.onerror = () => {
      if (disposed) return;
      disposed = true;
      setError('Delegate history is unavailable. Close and reopen to retry.');
      stream.close();
    };
    return () => { disposed = true; stream.close(); };
  }, [expanded, cyclic, reference.session_id, reference.turn_id]);
  return <details onToggle={event => setExpanded(event.currentTarget.open)}>
    <summary>Delegate: {reference.role} — {reference.status}</summary>
    {expanded && (cyclic ? <p role="alert">This execution reference forms a cycle.</p> : <>
      {!state.messages.length && !error && <p role="status">Loading delegate history…</p>}
      <IRTranscript state={state} ancestors={ancestors} />
      {error && <p role="alert">{error}</p>}
    </>)}
  </details>;
}

export default function IRTranscript({ state, onReply, ancestors = [], controlsReady = true, messageLimit, pendingReplies }: {
  state: SessionState; onReply?: ReplyControl; ancestors?: string[]; controlsReady?: boolean; messageLimit?: number; pendingReplies?: ReadonlySet<string>;
}) {
  const { byCall, matched } = toolResults(state);
  const messages = messageLimit === undefined ? state.messages : state.messages.slice(-messageLimit);
  const visibleTurns = new Set(messages.map(message => message.turn));
  const messageTurns = new Set(state.messages.map(message => message.turn));
  const pendingControls = controlsReady ? Object.entries(state.controls).filter(([, control]) => !control.response && !control.closed) : [];
  const resolvedControls = Object.entries(state.controls).filter(([, control]) =>
    (control.response || control.closed) && (messageLimit === undefined || visibleTurns.has(control.turn) || !messageTurns.has(control.turn)));
  return <>
    {messages.flatMap(m => m.blocks.map((b, index) => {
      const key = toolBlockKey(m, index);
      switch (b.type) {
        case 'text': {
          if (b.message_group && index > 0 && m.blocks[index - 1].type === 'text' && m.blocks[index - 1].message_group === b.message_group) return null;
          let text = b.text || '';
          if (b.message_group) for (let i = index + 1; i < m.blocks.length && m.blocks[i].type === 'text' && m.blocks[i].message_group === b.message_group; i++) text += m.blocks[i].text || '';
          return <Message key={key} role={m.role === 'user' ? 'user' : 'assistant'} text={text} streaming={!m.complete} />;
        }
        case 'thinking': return <ThinkingBlock key={key} text={b.text || ''} />;
        case 'tool_use': {
          const output = byCall.get(key);
          return <ToolBlock key={key} name={b.namespace ? `${b.namespace}/${b.name}` : b.name || 'Tool'} args={pretty(b.input ?? m.argumentFragments[index] ?? '')}
            result={output ? output.map(r => `${r.is_error ? 'Error: ' : ''}${pretty(r.content)}`).join('\n') :
              state.executions[m.turn] === 'cancelled' ? 'Turn cancelled' :
              state.executions[m.turn] === 'failed' ? 'Turn failed before a result was recorded' :
              state.executions[m.turn] === 'completed' ? 'No result reported by the harness' : m.error} />;
        }
        case 'tool_result': return matched.has(key) ? null : <ToolBlock key={key} name={b.tool_id || 'Tool result'} args="" result={pretty(b.content)} />;
        case 'image': {
          const url = mediaURL(b.media_ref);
          return url ? <img key={key} src={url} alt="Conversation attachment" style={{ maxWidth: '80%' }} /> : <pre key={key}>{b.media_ref}</pre>;
        }
        case 'document': {
          const url = mediaURL(b.media_ref);
          return url ? <a key={key} href={url} target="_blank" rel="noreferrer">Document ({b.media_type})</a> : <pre key={key}>{b.media_ref}</pre>;
        }
      }
    }))}
    {pendingControls.map(([id, control]) => <ControlCard key={`${state.session}:${id}`} id={id} control={control} onReply={onReply} replyPending={pendingReplies?.has(id)} />)}
    {resolvedControls.length > 0 && <details className="ir-resolved-controls">
      <summary>Resolved requests ({resolvedControls.length})</summary>
      {resolvedControls.map(([id, control]) => <ControlCard key={`${state.session}:${id}`} id={id} control={control} />)}
    </details>}
    {Object.values(state.children ?? {}).map(child => <ChildExecutionView
      key={`${child.session_id}:${child.turn_id}`} reference={child} ancestors={[...ancestors, state.session]} />)}
    {(state.projections ?? []).map(report => <p role="status" key={report.id}>
      {report.omitted_reasoning_blocks > 0 && <>
        {report.omitted_reasoning_blocks} reasoning {report.omitted_reasoning_blocks === 1 ? 'block remains' : 'blocks remain'} in history and
        {report.omitted_reasoning_blocks === 1 ? ' was' : ' were'} omitted from {report.boundary === 'request' ? `${report.model}'s input` : `the client response from ${report.model}`}.
        {report.omitted_opaque_state_blocks > 0 && <> {report.omitted_opaque_state_blocks} {report.omitted_opaque_state_blocks === 1 ? 'contains' : 'contain'} native opaque state.</>}
      </>}
      {(report.omitted_cache_controls ?? 0) > 0 && <> {report.omitted_cache_controls} requested cache {report.omitted_cache_controls === 1 ? 'setting was' : 'settings were'} not applied for {report.model}.</>}
    </p>)}
    {state.errors.map((error, i) => <div role="alert" key={`error:${i}`}>{error}</div>)}
  </>;
}
