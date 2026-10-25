import { parseJSON, stringifyJSON } from './json';
import { toolInterruptionText } from './interruption';
import { useEffect, useRef, useState } from 'react';
import { Message, ThinkingBlock, ToolBlock } from '../pages/chat/ChatPrimitives';
import { emptySession, reduceSession, toolResults, toolBlockKey } from './session';
import type { ChildExecution, Envelope, SessionState } from './session';
import ControlCard, { type ReplyControl } from './ControlCard';
import ImportReportView from './ImportReportView';
import ImportedCheckpointView from './ImportedCheckpointView';
const pretty = (value: unknown) => typeof value === 'string' ? value : stringifyJSON(value, 2) ?? '';
const incompleteResponse = 'Provider stream failed canonical admission or ended before completion';
function ExecutionError({ error }: { error: string }) {
  if (error === incompleteResponse) return <>The provider did not return a complete response.</>;
  try {
    const value = parseJSON(error) as Record<string, unknown>;
    const keys = ['reason', 'dispatched', 'submitted_input_sha256', 'submitted_input_bytes', 'submitted_input_preview', 'submitted_input_truncated'];
    if (value && typeof value === 'object' && Object.keys(value).length === keys.length && keys.every(key => Object.prototype.hasOwnProperty.call(value, key)) &&
        value.reason === 'Steering continuation was not dispatched; no model or tool executed. Send it again explicitly.' && value.dispatched === false &&
        typeof value.submitted_input_sha256 === 'string' && /^[a-f0-9]{64}$/.test(value.submitted_input_sha256) &&
        typeof value.submitted_input_bytes === 'number' && Number.isSafeInteger(value.submitted_input_bytes) && value.submitted_input_bytes > 0 &&
        typeof value.submitted_input_preview === 'string' && new TextEncoder().encode(value.submitted_input_preview).length <= 1536 &&
        typeof value.submitted_input_truncated === 'boolean' &&
        (value.submitted_input_truncated ? new TextEncoder().encode(value.submitted_input_preview).length < value.submitted_input_bytes :
          new TextEncoder().encode(value.submitted_input_preview).length === value.submitted_input_bytes)) {
      return <><p>{value.reason}</p><p>{value.submitted_input_truncated ? 'Submitted text (shortened):' : 'Submitted text:'}</p>
        <blockquote style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere' }}>{value.submitted_input_preview}</blockquote></>;
    }
  } catch { /* Ordinary or malformed diagnostics retain their exact fallback. */ }
  return <>{error}</>;
}
function mediaURL(ref: string | undefined): string | undefined {
  if (ref && (/^https?:\/\//i.test(ref) || /^data:image\/(png|jpeg|webp|gif);base64,/i.test(ref) || ref.startsWith('/api/artifacts/'))) return ref;
  return undefined;
}
function ChildExecutionView({ reference, ancestors, onReply, pendingRepliesBySession }: {
  reference: ChildExecution; ancestors: string[]; onReply?: ReplyControl;
  pendingRepliesBySession?: Readonly<Record<string, ReadonlySet<string>>>;
}) {
  const [expanded, setExpanded] = useState(false);
  const current = useRef(emptySession(reference.session_id));
  const [state, setState] = useState(current.current);
  const [error, setError] = useState('');
  const [ready, setReady] = useState(false);
  const readyRef = useRef(false);
  const expandedRef = useRef(false);
  const ownerRef = useRef(reference);
  ownerRef.current = reference;
  const cyclic = ancestors.includes(reference.session_id);
  useEffect(() => {
    if (!expanded || cyclic) return;
    setError('');
    readyRef.current = false;
    setReady(false);
    let disposed = false;
    const stream = new EventSource(`/api/chat/session-events?format=ir&sid=${encodeURIComponent(reference.session_id)}&cursor=${current.current.sequence}`);
    stream.addEventListener('ir', (message) => {
      if (disposed) return;
      try {
        const envelope = parseJSON((message as MessageEvent).data) as Envelope;
        const next = reduceSession(current.current, envelope);
        current.current = next;
        setState(next);
        // Post-terminal lifecycle metadata can follow the execution terminal.
        // The expanded view owns this read-only stream until collapse/unmount.
      } catch {
        disposed = true;
        readyRef.current = false;
        setReady(false);
        setError('Delegate history could not be read. Close and reopen to retry.');
        stream.close();
      }
    });
    stream.addEventListener('replay_complete', (message) => {
      if (disposed) return;
      try {
        const boundary = parseJSON((message as MessageEvent).data) as { session_id?: string; cursor?: string };
        if (boundary.session_id !== reference.session_id || typeof boundary.cursor !== 'string' ||
            !/^(0|[1-9][0-9]*)$/.test(boundary.cursor) || BigInt(boundary.cursor) > BigInt(current.current.sequence))
          throw new Error('Invalid delegate replay boundary');
        readyRef.current = true;
        setReady(true);
      } catch {
        disposed = true;
        readyRef.current = false;
        setReady(false);
        setError('Delegate history could not be read. Close and reopen to retry.');
        stream.close();
      }
    });
    stream.onerror = () => {
      if (disposed) return;
      disposed = true;
      readyRef.current = false;
      setReady(false);
      setError('Delegate history is unavailable. Close and reopen to retry.');
      stream.close();
    };
    return () => { disposed = true; readyRef.current = false; stream.close(); };
  }, [expanded, cyclic, reference.session_id, reference.turn_id]);
  return <details onToggle={event => { expandedRef.current = event.currentTarget.open; setExpanded(event.currentTarget.open); }}>
    <summary>Delegate: {reference.role} — {reference.status}</summary>
    {expanded && (cyclic ? <p role="alert">This execution reference forms a cycle.</p> : <>
      {!state.messages.length && !error && <p role="status">Loading delegate history…</p>}
      <IRTranscript state={state} ancestors={ancestors} controlsReady={ready}
        onReply={onReply ? async (id, reply, session = reference.session_id) => {
          if (!expandedRef.current || !readyRef.current || ownerRef.current.session_id !== reference.session_id ||
              ownerRef.current.turn_id !== reference.turn_id || current.current.session !== reference.session_id)
            throw new Error('Reopen this delegate history before replying.');
          // Each descendant validates its own pending control. Ancestors retain
          // live ownership/readiness checks without looking it up in the wrong log.
          if (session === reference.session_id) {
            const control = current.current.controls[id];
            if (!control || control.closed || control.response || control.turn !== reference.turn_id)
              throw new Error('This delegate request is no longer awaiting a reply.');
          }
          return onReply(id, reply, session);
        } : undefined}
        pendingReplies={pendingRepliesBySession?.[reference.session_id]} pendingRepliesBySession={pendingRepliesBySession} />
      {error && <p role="alert">{error}</p>}
    </>)}
  </details>;
}

function ProjectionNotice({ report }: { report: SessionState['projections'][number] }) {
  return <>
    {report.omitted_reasoning_blocks > 0 && <>
        {report.omitted_reasoning_blocks} reasoning {report.omitted_reasoning_blocks === 1 ? 'block remains' : 'blocks remain'} in history and
        {report.omitted_reasoning_blocks === 1 ? ' was' : ' were'} omitted from {report.boundary === 'request' ? `${report.model}'s input` : `the client response from ${report.model}`}.
        {report.omitted_opaque_state_blocks > 0 && <> {report.omitted_opaque_state_blocks} {report.omitted_opaque_state_blocks === 1 ? 'contains' : 'contain'} native opaque state.</>}
      </>}
    {(report.host_context_blocks_as_text ?? 0) > 0 && <> ACP sends host instructions as context ({report.host_context_blocks_as_text} {report.host_context_blocks_as_text === 1 ? 'block' : 'blocks'}).</>}
    {(report.history_messages_as_text ?? 0) > 0 && <> ACP receives earlier messages as text context ({report.history_messages_as_text}).</>}
    {(report.unverified_external_tool_reports ?? 0) > 0 && <> External tool reports are unverified ({report.unverified_external_tool_reports}).</>}
    {(report.omitted_request_controls ?? 0) > 0 && <> {report.model} could not apply {report.omitted_request_controls} requested {report.omitted_request_controls === 1 ? 'setting' : 'settings'}.</>}
    {(report.omitted_cache_controls ?? 0) > 0 && <> {report.omitted_cache_controls} requested cache {report.omitted_cache_controls === 1 ? 'setting was' : 'settings were'} not applied for {report.model}.</>}
  </>;
}
export default function IRTranscript({ state, onReply, ancestors = [], controlsReady = true, messageLimit, pendingReplies, pendingRepliesBySession, historicalRoles = false }: {
  state: SessionState; historicalRoles?: boolean; onReply?: ReplyControl; ancestors?: string[]; controlsReady?: boolean; messageLimit?: number; pendingReplies?: ReadonlySet<string>;
  pendingRepliesBySession?: Readonly<Record<string, ReadonlySet<string>>>;
}) {
  const { byCall, matched } = toolResults(state);
  const messages = messageLimit === undefined ? state.messages : state.messages.slice(-messageLimit);
  const visibleTurns = new Set(messages.map(message => message.turn));
  const messageTurns = new Set(state.messages.map(message => message.turn));
  const pendingControls = controlsReady ? Object.entries(state.controls).filter(([, control]) => !control.response && !control.closed) : [];
  const resolvedControls = Object.entries(state.controls).filter(([, control]) =>
    (control.response || control.closed) && (messageLimit === undefined || visibleTurns.has(control.turn) || !messageTurns.has(control.turn)));
  // A stream diagnostic and its failed terminal describe one turn. Retain both
  // observations, but give the terminal's actionable explanation priority.
  const secondary = new Map<number, number[]>();
  const folded = new Set<number>();
  const diagnosticsByTurn = new Map<string, number[]>();
  state.errors.forEach((error, index) => {
    const source = state.errorOrigins?.[index];
    if (error !== incompleteResponse || !source?.turn || source.terminal) return;
    const indices = diagnosticsByTurn.get(source.turn) ?? [];
    indices.push(index);
    diagnosticsByTurn.set(source.turn, indices);
  });
  state.errors.forEach((error, index) => {
    const origin = state.errorOrigins?.[index];
    const terminal = origin && state.terminalSources[origin.turn];
    if (!origin?.turn || !origin.terminal || !error.trim() || terminal?.status !== 'failed' || terminal.sequence !== origin.sequence ||
        terminal.error !== error || error === incompleteResponse) return;
    const details = (diagnosticsByTurn.get(origin.turn) ?? []).filter(prior =>
      BigInt(state.errorOrigins![prior].sequence) < BigInt(origin.sequence));
    if (details.length) { secondary.set(index, details); details.forEach(prior => folded.add(prior)); }
  });
  const lastMessageByTurn = new Map(messages.map(message => [message.turn, message.id]));
  const errorsByTurn = new Map<string, number[]>();
  const unanchoredErrors: number[] = [];
  state.errors.forEach((_, index) => {
    if (folded.has(index)) return;
    const turn = state.errorOrigins?.[index]?.turn;
    if (turn && lastMessageByTurn.has(turn)) {
      const indices = errorsByTurn.get(turn) ?? [];
      indices.push(index);
      errorsByTurn.set(turn, indices);
    } else unanchoredErrors.push(index);
  });
  const renderDiagnostic = (index: number, earlier = false) => <div key={`error:${index}`}>
    {earlier && <p className="text-xs text-muted-foreground">Earlier turn</p>}
    <div role="alert"><ExecutionError error={state.errors[index]} /></div>
    {secondary.has(index) && <details className="ir-response-details">
      <summary>Response details</summary>
      {secondary.get(index)!.map(prior => <p key={prior}><ExecutionError error={state.errors[prior]} /></p>)}
    </details>}
  </div>;
  return <>
    {(state.importReports ?? []).map(observation => <section key={observation.id}>
      <ImportReportView observation={observation} />
      <ImportedCheckpointView observation={observation} renderMessages={imported => <IRTranscript historicalRoles controlsReady={false}
        state={{ ...emptySession(state.session), messages: imported.map((message, index) => ({ ...message, id: `${observation.id}:message:${index}`,
          turn: observation.turn, complete: true, argumentFragments: {}, stoppedBlocks: message.blocks.map((_, i) => i) })) }} />} />
    </section>)}
    {messages.flatMap(m => [...(historicalRoles ? [<p key={`${m.id}:role`} className="text-xs text-muted-foreground">{m.role}</p>] : []), ...m.blocks.map((b, index) => {
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
          const interruption = state.toolInterruptions[key];
          return <ToolBlock key={key} name={b.namespace ? `${b.namespace}/${b.name}` : b.name || 'Tool'} args={pretty(b.input ?? m.argumentFragments[index] ?? '')}
            unknownOutcome={interruption ? toolInterruptionText : undefined}
            result={interruption ? undefined : output ? output.map(r => `${r.is_error ? 'Error: ' : ''}${pretty(r.content)}`).join('\n') :
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
    }), ...(m.role === 'assistant' && (m.stopReason === 'content_filter' || m.nativeStopReason === 'refusal') ? [<details key={`${m.id}:refusal`} className="ir-provider-refusal">
      <summary>Provider refusal</summary>
      <p>The provider declined to complete this response.</p>
      {m.refusal?.category !== null && m.refusal?.category !== undefined && <p style={{ overflowWrap: 'anywhere' }}>Category: <span>{m.refusal.category}</span></p>}
      {m.refusal?.explanation !== null && m.refusal?.explanation !== undefined && <p style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere' }}>{m.refusal.explanation}</p>}
    </details>] : []), ...(lastMessageByTurn.get(m.turn) === m.id
      ? (errorsByTurn.get(m.turn) ?? []).map(index => renderDiagnostic(index)) : [])])}
    {pendingControls.map(([id, control]) => <ControlCard key={`${state.session}:${id}`} id={id} control={control} onReply={onReply} replyPending={pendingReplies?.has(id)} />)}
    {resolvedControls.length > 0 && <details className="ir-resolved-controls">
      <summary>Resolved requests ({resolvedControls.length})</summary>
      {resolvedControls.map(([id, control]) => <ControlCard key={`${state.session}:${id}`} id={id} control={control} />)}
    </details>}
    {Object.values(state.children ?? {}).map(child => <ChildExecutionView
      key={`${child.session_id}:${child.turn_id}`} reference={child} ancestors={[...ancestors, state.session]}
      onReply={onReply} pendingRepliesBySession={pendingRepliesBySession} />)}
    {(state.projections ?? []).length === 1 && <p role="status"><ProjectionNotice report={state.projections[0]} /></p>}
    {(state.projections ?? []).length > 1 && <div className="ir-compatibility-notices">
      <p role="status">
        {state.projections.some(report => report.omitted_reasoning_blocks > 0) && <>Some model inputs or responses omit reasoning that remains in history.</>}
        {state.projections.some(report => (report.omitted_request_controls ?? 0) > 0) && <> Some requested model settings could not be applied.</>}
        {state.projections.some(report => (report.host_context_blocks_as_text ?? 0) > 0) && <> ACP sends host instructions as context.</>}
        {state.projections.some(report => (report.history_messages_as_text ?? 0) > 0) && <> ACP receives earlier messages as text context.</>}
        {state.projections.some(report => (report.unverified_external_tool_reports ?? 0) > 0) && <> External tool reports are unverified.</>}
        {state.projections.some(report => (report.omitted_cache_controls ?? 0) > 0) && <> Some requested cache settings were not applied.</>}
      </p>
      <details>
        <summary>Model compatibility details ({state.projections.length})</summary>
        {state.projections.map(report => <p key={report.id}><ProjectionNotice report={report} /></p>)}
      </details>
    </div>}
    {unanchoredErrors.map(index => {
      const turn = state.errorOrigins?.[index]?.turn;
      return renderDiagnostic(index, !!turn && messageTurns.has(turn));
    })}
  </>;
}
