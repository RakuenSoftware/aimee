import { Message, ThinkingBlock, ToolBlock } from '../pages/chat/ChatPrimitives';
import { toolResults } from './session';
import type { SessionState } from './session';
const pretty = (value: unknown) => typeof value === 'string' ? value : JSON.stringify(value, null, 2) ?? '';
function mediaURL(ref: string | undefined): string | undefined {
  if (ref && (/^https?:\/\//i.test(ref) || /^data:image\/(png|jpeg|webp|gif);base64,/i.test(ref) || ref.startsWith('/api/artifacts/'))) return ref;
  return undefined;
}
export default function IRTranscript({ state }: { state: SessionState }) {
  const results = toolResults(state);
  const uses = new Set(state.messages.flatMap(m => m.blocks.filter(b => b.type === 'tool_use').map(b => b.tool_id)));
  return <>
    {state.messages.flatMap(m => m.blocks.map((b, index) => {
      const key = `${m.id}:${index}`;
      switch (b.type) {
        case 'text': return <Message key={key} role={m.role === 'user' ? 'user' : 'assistant'} text={b.text || ''} streaming={!m.complete} />;
        case 'thinking': return <ThinkingBlock key={key} text={b.text || ''} />;
        case 'tool_use': {
          const output = results.get(b.tool_id || '');
          return <ToolBlock key={key} name={b.namespace ? `${b.namespace}/${b.name}` : b.name || 'Tool'} args={pretty(b.input ?? m.argumentFragments[index] ?? '')}
            result={output ? output.map(r => `${r.is_error ? 'Error: ' : ''}${pretty(r.content)}`).join('\n') :
              state.executions[m.turn] === 'cancelled' ? 'Turn cancelled' :
              state.executions[m.turn] === 'failed' ? 'Turn failed before a result was recorded' :
              state.executions[m.turn] === 'completed' ? 'No result reported by the harness' : m.error} />;
        }
        case 'tool_result': return uses.has(b.tool_id) ? null : <ToolBlock key={key} name={b.tool_id || 'Tool result'} args="" result={pretty(b.content)} />;
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
    {Object.entries(state.controls).map(([id, c]) => <details key={id}>
      <summary>{c.kind}: {c.response ? 'Resolved' : c.closed ? 'Closed' : 'Awaiting operator'}</summary>
      <pre>{pretty(c.request)}</pre>{c.response && <pre>{pretty(c.response)}</pre>}
    </details>)}
    {state.errors.map((error, i) => <div role="alert" key={`error:${i}`}>{error}</div>)}
  </>;
}
