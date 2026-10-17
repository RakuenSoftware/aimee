/** Canonical session projection. No native provider/harness protocol is accepted
 * here. Live delivery and durable replay use exactly the same reducer. */
export type Block = {
  type: 'text' | 'thinking' | 'tool_use' | 'tool_result' | 'image' | 'document';
  text?: string;
  signature?: string;
  tool_id?: string;
  name?: string;
  namespace?: string;
  input?: unknown;
  content?: unknown;
  is_error?: boolean;
  media_type?: string;
  media_ref?: string;
  context?: Record<string, unknown>;
};
export type Message = { role: string; blocks: Block[] };
export type Delta = {
  type: 'TURN_START' | 'BLOCK_START' | 'BLOCK_DELTA' | 'BLOCK_STOP' | 'TURN_STOP' | 'ERROR';
  block_id?: number;
  kind?: Block['type'];
  tool_id?: string;
  name?: string;
  text_delta?: string;
  tool_args_delta?: string;
  stop_reason?: string;
  usage_in?: number;
  usage_out?: number;
  error_message?: string;
};
export type SessionEvent =
  | { kind: 'message'; message_id: string; payload: Message }
  | { kind: 'delta'; message_id: string; role: string; payload: Delta }
  | { kind: 'execution'; payload: { status: 'running' | 'completed' | 'failed' | 'cancelled'; error?: string } }
  | { kind: 'control_request' | 'control_response'; request_id: string;
      control_kind: string; payload: Record<string, unknown> };
export type Envelope = {
  version: 1; session_id: string; sequence: string; event_id: string;
  turn_id: string; actor: string; event: SessionEvent;
};
export type ProjectedMessage = Message & {
  id: string; turn: string; complete: boolean;
  argumentFragments: Record<number, string>;
  stoppedBlocks: number[];
  error?: string;
  cancelled?: boolean;
  stopReason?: string;
  usage?: { input: number; output: number };
};
export type Control = {
  kind: string; request: Record<string, unknown>; response?: Record<string, unknown>;
  requestedBy: string; resolvedBy?: string; turn: string; closed?: boolean;
};
export type SessionState = {
  session: string; sequence: string; messages: ProjectedMessage[];
  controls: Record<string, Control>; activeTurns: string[]; errors: string[];
  executions: Record<string, 'running' | 'completed' | 'failed' | 'cancelled'>;
};
export function emptySession(session: string): SessionState {
  return { session, sequence: '0', messages: [], controls: {}, activeTurns: [], errors: [], executions: {} };
}
function sequence(value: string): bigint {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) throw new Error('Invalid session sequence');
  return BigInt(value);
}
const kinds = new Set(['text', 'thinking', 'tool_use', 'tool_result', 'image', 'document']);
function nonempty(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0;
}
function validateBlock(block: Block): void {
  if (!block || !kinds.has(block.type)) throw new Error('Unsupported canonical block');
  if ((block.type === 'text' || block.type === 'thinking') && typeof block.text !== 'string')
    throw new Error('Text block requires text');
  if (block.signature !== undefined && typeof block.signature !== 'string')
    throw new Error('Invalid thinking signature');
  if ((block.type === 'tool_use' || block.type === 'tool_result') && !nonempty(block.tool_id))
    throw new Error('Tool block requires tool_id');
  if (block.type === 'tool_use' && !nonempty(block.name)) throw new Error('Tool use requires name');
  if (block.is_error !== undefined && typeof block.is_error !== 'boolean')
    throw new Error('Invalid tool result status');
  if ((block.type === 'image' || block.type === 'document') &&
      (!nonempty(block.media_type) || !nonempty(block.media_ref))) throw new Error('Invalid media reference');
}
export function reduceSession(state: SessionState, envelope: Envelope): SessionState {
  if (envelope.version !== 1 || envelope.session_id !== state.session || !nonempty(envelope.event_id) ||
      !nonempty(envelope.turn_id) || !nonempty(envelope.actor)) throw new Error('Invalid session envelope');
  const nextSequence = sequence(envelope.sequence);
  const previousSequence = sequence(state.sequence);
  if (nextSequence <= previousSequence) return state; // exact durable replay cursor
  if (nextSequence !== previousSequence + 1n) throw new Error('Session replay gap');
  const event = envelope.event;
  const next: SessionState = { ...state, sequence: envelope.sequence };
  const execution = state.executions[envelope.turn_id];
  if (execution && execution !== 'running' &&
      (event.kind === 'message' || event.kind === 'delta' || event.kind === 'control_request'))
    throw new Error('Content follows execution completion');
  switch (event.kind) {
    case 'message': {
      if (!nonempty(event.message_id) || !nonempty(event.payload.role) || !Array.isArray(event.payload.blocks) || event.payload.blocks.length > 4096)
        throw new Error('Invalid canonical message');
      event.payload.blocks.forEach(validateBlock);
      const message: ProjectedMessage = { ...event.payload, id: event.message_id,
        turn: envelope.turn_id, complete: true, argumentFragments: {}, stoppedBlocks: [] };
      const index = state.messages.findIndex(m => m.id === event.message_id);
      if (index >= 0) {
        if (state.messages[index].role !== message.role || state.messages[index].turn !== message.turn)
          throw new Error('Message identity changed');
        const previous = state.messages[index];
        message.usage = previous.usage;
        message.stopReason = previous.stopReason;
        message.error = previous.error;
        message.cancelled = previous.cancelled;
        next.messages = state.messages.map((m, i) => i === index ? message : m);
      } else next.messages = [...state.messages, message];
      return next;
    }
    case 'delta': {
      if (!nonempty(event.message_id) || !nonempty(event.role)) throw new Error('Delta requires message identity');
      const d = event.payload;
      const index = state.messages.findIndex(m => m.id === event.message_id);
      if (d.type === 'ERROR') {
        const error = d.error_message || 'Runtime error';
        next.errors = [...state.errors, error];
        if (index >= 0) {
          const message = state.messages[index];
          if (message.role !== event.role || message.turn !== envelope.turn_id)
            throw new Error('Delta changed message identity');
          next.messages = state.messages.map((m, i) => i === index ? { ...m, complete: true, error } : m);
        }
        return next;
      }
      if (d.type === 'TURN_START') {
        if (index >= 0) throw new Error('Duplicate message start');
        if (index < 0) next.messages = [...state.messages, { id: event.message_id,
          turn: envelope.turn_id, role: event.role, blocks: [], complete: false,
          argumentFragments: {}, stoppedBlocks: [] }];
        return next;
      }
      if (index < 0) throw new Error('Delta precedes message start');
      const previous = state.messages[index];
      if (previous.role !== event.role || previous.turn !== envelope.turn_id)
        throw new Error('Delta changed message identity');
      if (previous.complete) throw new Error('Delta follows message completion');
      const message = { ...previous, blocks: [...previous.blocks],
        argumentFragments: { ...previous.argumentFragments }, stoppedBlocks: [...previous.stoppedBlocks] };
      next.messages = state.messages.map((m, i) => i === index ? message : m);
      if (d.type === 'TURN_STOP') {
        if (message.stoppedBlocks.length !== message.blocks.length)
          throw new Error('Message has open blocks');
        for (const value of [d.usage_in, d.usage_out]) {
          if (value !== undefined && (!Number.isSafeInteger(value) || value < 0))
            throw new Error('Invalid model usage');
        }
        message.usage = { input: d.usage_in || 0, output: d.usage_out || 0 };
        message.stopReason = d.stop_reason;
        message.complete = true;
        return next;
      }
      const blockID = d.block_id;
      if (blockID === undefined || !Number.isSafeInteger(blockID) || blockID < 0 || blockID > 4095)
        throw new Error('Invalid block identity');
      if (d.type === 'BLOCK_START') {
        if (!d.kind || !['text', 'thinking', 'tool_use'].includes(d.kind) || message.blocks[blockID]) throw new Error('Invalid block start');
        const block: Block = { type: d.kind, tool_id: d.tool_id, name: d.name,
          ...((d.kind === 'text' || d.kind === 'thinking') ? { text: '' } : {}) };
        validateBlock(block);
        // Sparse block indices are not valid canonical message order.
        if (blockID !== message.blocks.length) throw new Error('Block order gap');
        message.blocks.push(block);
        return next;
      }
      const block = message.blocks[blockID];
      if (!block || message.stoppedBlocks.includes(blockID)) throw new Error('Block is not open');
      if (d.type === 'BLOCK_DELTA') {
        if (d.kind && d.kind !== block.type) throw new Error('Block kind changed');
        const updated = { ...block };
        if (d.text_delta !== undefined) {
          if (typeof d.text_delta !== 'string') throw new Error('Invalid text delta');
          if (block.type !== 'text' && block.type !== 'thinking') throw new Error('Text in non-text block');
          updated.text = (block.text || '') + d.text_delta;
        }
        if (d.tool_args_delta !== undefined) {
          if (typeof d.tool_args_delta !== 'string') throw new Error('Invalid argument delta');
          if (block.type !== 'tool_use') throw new Error('Arguments in non-tool block');
          message.argumentFragments[blockID] = (message.argumentFragments[blockID] || '') + d.tool_args_delta;
        }
        message.blocks[blockID] = updated;
        return next;
      }
      if (d.type === 'BLOCK_STOP') {
        if (block.type === 'tool_use') message.blocks[blockID] = { ...block,
          input: JSON.parse(message.argumentFragments[blockID] || '{}') as unknown };
        message.stoppedBlocks.push(blockID);
        return next;
      }
      throw new Error('Unsupported canonical delta');
    }
    case 'execution': {
      const status = event.payload.status;
      if (!['running', 'completed', 'failed', 'cancelled'].includes(status))
        throw new Error('Invalid execution status');
      const previous = state.executions[envelope.turn_id];
      if (previous && previous !== 'running') throw new Error('Execution is already terminal');
      if (!previous && status !== 'running') throw new Error('Execution has not started');
      if (previous === 'running' && status === 'running') throw new Error('Duplicate execution start');
      next.executions = { ...state.executions, [envelope.turn_id]: status };
      next.activeTurns = status === 'running' ? [...state.activeTurns, envelope.turn_id] :
        state.activeTurns.filter(turn => turn !== envelope.turn_id);
      if (event.payload.error) next.errors = [...state.errors, event.payload.error];
      if (status !== 'running') {
        const open = state.messages.filter(m => m.turn === envelope.turn_id && !m.complete);
        if (status === 'completed' && open.length) throw new Error('Execution completed with open messages');
        next.messages = state.messages.map(m => m.turn === envelope.turn_id && !m.complete ?
          { ...m, complete: true, cancelled: status === 'cancelled',
            error: status === 'failed' ? (event.payload.error || 'Execution failed') : undefined } : m);
        next.controls = Object.fromEntries(Object.entries(state.controls).map(([id, control]) =>
          [id, control.turn === envelope.turn_id && !control.response ? { ...control, closed: true } : control]));
      }
      return next;
    }
    case 'control_request': {
      if (!event.request_id || !event.control_kind || state.controls[event.request_id])
        throw new Error('Invalid or duplicate control request');
      next.controls = { ...state.controls, [event.request_id]: {
        kind: event.control_kind, request: event.payload, requestedBy: envelope.actor, turn: envelope.turn_id } };
      return next;
    }
    case 'control_response': {
      const request = state.controls[event.request_id];
      if (!request || request.response || request.closed || request.turn !== envelope.turn_id || request.kind !== event.control_kind)
        throw new Error('Control response has no matching pending request');
      next.controls = { ...state.controls, [event.request_id]: { ...request,
        response: event.payload, resolvedBy: envelope.actor } };
      return next;
    }
    default: throw new Error('Unsupported canonical event');
  }
}
/** Tools correlate across role/message boundaries and completion order. */
export function toolResults(state: SessionState): Map<string, Block[]> {
  const results = new Map<string, Block[]>();
  for (const message of state.messages) for (const block of message.blocks) {
    if (block.type !== 'tool_result' || !block.tool_id) continue;
    results.set(block.tool_id, [...(results.get(block.tool_id) || []), block]);
  }
  return results;
}
