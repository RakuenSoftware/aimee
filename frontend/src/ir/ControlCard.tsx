import { stringifyJSON } from './json';
import { useRef, useState } from 'react';
import './ControlCard.css';
import type { Control } from './session';
import ElicitationForm, { type FormContent } from './ElicitationForm';
export type ControlReply = { decision: 'allow' | 'deny' | 'cancel'; answers?: Record<string, string[]>; content?: FormContent };
export type ReplyControl = (requestId: string, reply: ControlReply, sessionId?: string) => Promise<void>;
type Question = { id: string; question: string; multiple: boolean; options: { label: string; description?: string }[] };
function unknownEffectAction(id: string, control: Control): { tool: string; arguments: Record<string, unknown>; count: number } | undefined {
  if (control.kind !== 'permission' || !/(?:^|:)unknown-effect:[a-f0-9]{64}$/.test(id)) return undefined;
  const object = (value: unknown): value is Record<string, unknown> => !!value && typeof value === 'object' && !Array.isArray(value);
  const input = control.request.input;
  if (!object(input) || Object.keys(input).length !== 8 ||
      !['session_id', 'turn_id', 'actor', 'request_id', 'attempt_id', 'tool'].every(key => typeof input[key] === 'string' && !!input[key]) ||
      input.tool !== control.request.tool || !object(input.arguments) || !object(input.unknown_receipts)) return undefined;
  const summary = input.unknown_receipts;
  if (Object.keys(summary).length !== 4 || typeof summary.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(summary.sha256) ||
      typeof summary.count !== 'number' || !Number.isInteger(summary.count) || summary.count < 1 || summary.count > 4096 ||
      !Array.isArray(summary.sources) || summary.sources.length > 4 || summary.sources.length > summary.count || summary.displayed !== summary.sources.length ||
      !summary.sources.every(source => object(source) && Object.keys(source).length === 6 &&
        ['event_id', 'session_id', 'turn_id', 'sequence', 'tool_id', 'name'].every(key => typeof source[key] === 'string' && !!source[key]))) return undefined;
  return { tool: input.tool as string, arguments: input.arguments, count: summary.count };
}
function questions(request: Record<string, unknown>): Question[] {
  if (!Array.isArray(request.questions)) return [];
  return request.questions.filter((q): q is Question => !!q && typeof q === 'object' &&
    typeof q.id === 'string' && typeof q.question === 'string' && Array.isArray(q.options))
    .map(q => ({ ...q, options: q.options.filter(option => option && typeof option.label === 'string') }));
}
function elicitationURL(request: Record<string, unknown>): { href: string; host: string; message: string; server: string } | undefined {
  const value = request.url;
  if (!value || typeof value !== 'object' || request.form) return undefined;
  const url = value as Record<string, unknown>;
  const bytes = (value: string) => new TextEncoder().encode(value).length;
  if (Object.keys(url).length !== 4 || typeof url.href !== 'string' || bytes(url.href) > 8192 || /[\u0000-\u0020\u007f\\]/.test(url.href) ||
      typeof url.elicitation_id !== 'string' || !url.elicitation_id || bytes(url.elicitation_id) > 1024 ||
      typeof url.message !== 'string' || bytes(url.message) > 65535 ||
      typeof url.server !== 'string' || !url.server || bytes(url.server) > 256) return undefined;
  try {
    const parsed = new URL(url.href);
    if (!['http:', 'https:'].includes(parsed.protocol) || !parsed.hostname || parsed.username || parsed.password) return undefined;
    return { href: url.href, host: parsed.host, message: url.message, server: url.server };
  } catch { return undefined; }
}
export default function ControlCard({ id, control, onReply, replyPending = false }: { id: string; control: Control; onReply?: ReplyControl; replyPending?: boolean }) {
  const unknownAction = unknownEffectAction(id, control);
  const external = control.kind === 'elicitation' ? elicitationURL(control.request) : undefined;
  const [answers, setAnswers] = useState<Record<string, string[]>>({});
  const [custom, setCustom] = useState<Record<string, string>>({});
  const [sending, setPending] = useState(false);
  const pending = sending || replyPending;
  const submitting = useRef(false);
  const [error, setError] = useState('');
  const items = questions(control.request);
  const closed = !!control.response || !!control.closed;
  const finalAnswers = Object.fromEntries(items.map(q => [q.id, [...(answers[q.id] ?? []), ...(custom[q.id]?.trim() ? [custom[q.id].trim()] : [])]]));
  const canAnswer = control.kind === 'permission' || (control.kind === 'question' && items.length > 0 && items.every(q => finalAnswers[q.id].length > 0));
  async function submit(decision: ControlReply['decision'], content?: FormContent) {
    if (!onReply || closed || replyPending || submitting.current) return;
    submitting.current = true;
    setPending(true); setError('');
    try { await onReply(id, { decision, ...(decision === 'allow' && control.kind === 'question' ? { answers: finalAnswers } : {}), ...(decision === 'allow' && content ? { content } : {}) }); }
    catch (err) { submitting.current = false; setError(err instanceof Error ? err.message : 'Could not send reply'); setPending(false); }
  }
  return <details className="ir-control" data-closed={closed} open={!closed} aria-busy={pending && !closed}>
    <summary>{control.kind}: {control.response ? 'Resolved' : control.closed ? 'Closed' : pending ? 'Reply sent' : 'Awaiting your reply'}</summary>
    {typeof control.request.title === 'string' && <p>{control.request.title}</p>}
    {typeof control.request.description === 'string' && control.request.description !== control.request.title && <p>{control.request.description}</p>}
    {unknownAction ? <>
      <p>{unknownAction.count} earlier tool {unknownAction.count === 1 ? 'outcome remains' : 'outcomes remain'} unknown.</p>
      <p>Requested action: <strong>{unknownAction.tool}</strong></p>
      <pre>{stringifyJSON(unknownAction.arguments, 2)}</pre>
    </> : control.kind !== 'question' && control.kind !== 'elicitation' && <pre>{stringifyJSON(control.request.input ?? control.request, 2)}</pre>}
    {external && <div>
      <p>{external.server}: {external.message}</p>
      <p>Destination: {external.host}</p>
      {!closed && onReply && <a href={external.href} target="_blank" rel="noopener noreferrer" referrerPolicy="no-referrer"
        aria-disabled={pending} onClick={event => { if (submitting.current) event.preventDefault(); else void submit('allow'); }}>
        Allow and open {external.host}
      </a>}
      {control.response?.decision === 'allow' && <>
        <p>Opening consent recorded. Completion is reported by the external service.</p>
        <a href={external.href} target="_blank" rel="noopener noreferrer" referrerPolicy="no-referrer">Open {external.host} again</a>
      </>}
    </div>}
    {!closed && control.kind === 'elicitation' && !!control.request.form && typeof control.request.form === 'object' &&
      <ElicitationForm form={control.request.form as Record<string, unknown>} disabled={pending || !onReply} onSubmit={content => void submit('allow', content)} />}
    {!closed && items.map(q => <fieldset key={q.id} disabled={pending || !onReply}>
      <legend>{q.question}</legend>
      {q.options.map(option => <label key={option.label} style={{ display: 'block' }}>
        <input type={q.multiple ? 'checkbox' : 'radio'} name={`${id}:${q.id}`} value={option.label}
          checked={(answers[q.id] ?? []).includes(option.label)} onChange={event => {
            if (!q.multiple) setCustom(old => ({ ...old, [q.id]: '' }));
            setAnswers(old => ({ ...old,
              [q.id]: q.multiple ? event.target.checked ? [...(old[q.id] ?? []), option.label] : (old[q.id] ?? []).filter(value => value !== option.label) : [option.label],
            }));
          }} />{option.label}{option.description && <span> — {option.description}</span>}
      </label>)}
      <label>Your answer<input aria-label={`Your answer: ${q.question}`} value={custom[q.id] ?? ''}
        onChange={event => {
          if (!q.multiple) setAnswers(old => ({ ...old, [q.id]: [] }));
          setCustom(old => ({ ...old, [q.id]: event.target.value }));
        }} /></label>
    </fieldset>)}
    {!closed && onReply && <div className="ir-control-actions">
      {(control.kind === 'permission' || control.kind === 'question') && <button className="ir-control-submit" type="button" disabled={pending || !canAnswer} onClick={() => void submit('allow')}>{control.kind === 'question' ? 'Send answers' : 'Allow once'}</button>}
      <button type="button" disabled={pending} onClick={() => void submit('deny')}>Decline</button>
      <button type="button" disabled={pending} onClick={() => void submit('cancel')}>Cancel</button>
    </div>}
    {control.response && <details className="ir-control-response"><summary>Response details</summary><pre>{stringifyJSON(control.response, 2)}</pre></details>}
    {error && <p role="alert">{error}</p>}
  </details>;
}
