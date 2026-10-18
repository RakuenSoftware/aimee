import { Message, ThinkingBlock, ToolBlock } from '../pages/chat/ChatPrimitives';
import { toolResults } from './session';
import type { SessionState } from './session';
import ControlCard, { type ReplyControl } from './ControlCard';
const pretty = (value: unknown) => typeof value === 'string' ? value : JSON.stringify(value, null, 2) ?? '';
function mediaURL(ref: string | undefined): string | undefined {
  if (ref && (/^https?:\/\//i.test(ref) || /^data:image\/(png|jpeg|webp|gif);base64,/i.test(ref) || ref.startsWith('/api/artifacts/'))) return ref;
  return undefined;
}
export default function IRTranscript({ state, onReply }: { state: SessionState; onReply?: ReplyControl }) {
  const results = toolResults(state);
  const uses = new Set(state.messages.flatMap(m => m.blocks.filter(b => b.type === 'tool_use').map(b => b.tool_id)));
  return <>
    {state.messages.flatMap(m => m.blocks.map((b, index) => {
      const key = `${m.id}:${index}`;
      switch (b.type) {
        case 'text': {
          if (b.message_group && index > 0 && m.blocks[index - 1].type === 'text' && m.blocks[index - 1].message_group === b.message_group) return null;
          let text = b.text || '';
          if (b.message_group) for (let i = index + 1; i < m.blocks.length && m.blocks[i].type === 'text' && m.blocks[i].message_group === b.message_group; i++) text += m.blocks[i].text || '';
          return <Message key={key} role={m.role === 'user' ? 'user' : 'assistant'} text={text} streaming={!m.complete} />;
        }
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
    {Object.entries(state.controls).map(([id, control]) => <ControlCard key={id} id={id} control={control} onReply={onReply} />)}
    {(state.projections ?? []).map(report => <p role="status" key={report.id}>
      {report.omitted_reasoning_blocks} reasoning {report.omitted_reasoning_blocks === 1 ? 'block remains' : 'blocks remain'} in history and
      {report.omitted_reasoning_blocks === 1 ? ' was' : ' were'} omitted from {report.boundary === 'request' ? `${report.model}'s input` : `the client response from ${report.model}`}.
      {report.omitted_opaque_state_blocks > 0 && <> {report.omitted_opaque_state_blocks} {report.omitted_opaque_state_blocks === 1 ? 'contains' : 'contain'} native opaque state.</>}
    </p>)}
    {state.errors.map((error, i) => <div role="alert" key={`error:${i}`}>{error}</div>)}
  </>;
}
