import { useState } from 'react';
import type { Control } from './session';
export type ControlReply = { decision: 'allow' | 'deny' | 'cancel'; answers?: Record<string, string[]> };
export type ReplyControl = (requestId: string, reply: ControlReply) => Promise<void>;
type Question = { id: string; question: string; multiple: boolean; options: { label: string; description?: string }[] };
function questions(request: Record<string, unknown>): Question[] {
  if (!Array.isArray(request.questions)) return [];
  return request.questions.filter((q): q is Question => !!q && typeof q === 'object' &&
    typeof q.id === 'string' && typeof q.question === 'string' && Array.isArray(q.options))
    .map(q => ({ ...q, options: q.options.filter(option => option && typeof option.label === 'string') }));
}
export default function ControlCard({ id, control, onReply }: { id: string; control: Control; onReply?: ReplyControl }) {
  const [answers, setAnswers] = useState<Record<string, string[]>>({});
  const [custom, setCustom] = useState<Record<string, string>>({});
  const [pending, setPending] = useState(false);
  const [error, setError] = useState('');
  const items = questions(control.request);
  const closed = !!control.response || !!control.closed;
  const finalAnswers = Object.fromEntries(items.map(q => [q.id, [...(answers[q.id] ?? []), ...(custom[q.id]?.trim() ? [custom[q.id].trim()] : [])]]));
  const canAnswer = control.kind === 'permission' || (control.kind === 'question' && items.length > 0 && items.every(q => finalAnswers[q.id].length > 0));
  async function submit(decision: ControlReply['decision']) {
    if (!onReply || closed || pending) return;
    setPending(true); setError('');
    try { await onReply(id, { decision, ...(decision === 'allow' && control.kind === 'question' ? { answers: finalAnswers } : {}) }); }
    catch (err) { setError(err instanceof Error ? err.message : 'Could not send reply'); setPending(false); }
  }
  return <details open={!closed}>
    <summary>{control.kind}: {control.response ? 'Resolved' : control.closed ? 'Closed' : pending ? 'Reply sent' : 'Awaiting your reply'}</summary>
    {typeof control.request.title === 'string' && <p>{control.request.title}</p>}
    {control.kind !== 'question' && <pre>{JSON.stringify(control.request.input ?? control.request, null, 2)}</pre>}
    {!closed && items.map(q => <fieldset key={q.id} disabled={pending || !onReply}>
      <legend>{q.question}</legend>
      {q.options.map(option => <label key={option.label} style={{ display: 'block' }}>
        <input type={q.multiple ? 'checkbox' : 'radio'} name={`${id}:${q.id}`} value={option.label}
          checked={(answers[q.id] ?? []).includes(option.label)} onChange={event => setAnswers(old => ({ ...old,
            [q.id]: q.multiple ? event.target.checked ? [...(old[q.id] ?? []), option.label] : (old[q.id] ?? []).filter(value => value !== option.label) : [option.label],
          }))} />{option.label}{option.description && <span> — {option.description}</span>}
      </label>)}
      <label>Your answer<input aria-label={`Your answer: ${q.question}`} value={custom[q.id] ?? ''}
        onChange={event => setCustom(old => ({ ...old, [q.id]: event.target.value }))} /></label>
    </fieldset>)}
    {!closed && onReply && <div>
      {canAnswer && <button type="button" disabled={pending} onClick={() => void submit('allow')}>{control.kind === 'question' ? 'Send answers' : 'Allow once'}</button>}
      <button type="button" disabled={pending} onClick={() => void submit('deny')}>Decline</button>
    </div>}
    {control.response && <pre>{JSON.stringify(control.response, null, 2)}</pre>}
    {error && <p role="alert">{error}</p>}
  </details>;
}
