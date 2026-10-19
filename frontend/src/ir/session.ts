import { parseJSON } from './json';
/** Canonical session projection. No native provider/harness protocol is accepted
 * here. Live delivery and durable replay use exactly the same reducer. */
export type Block = {
  type: 'text' | 'thinking' | 'tool_use' | 'tool_result' | 'image' | 'document';
  text?: string;
  signature?: string;
  thinking_binding?: string;
  thinking_id?: string;
  encrypted_content?: string;
  redacted_content?: string;
  cache_control?: string;
  cache_ttl?: string;
  message_group?: number;
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
export type Message = { role: string; blocks: Block[]; content_scalar?: boolean };
export type Delta = {
  type: 'TURN_START' | 'BLOCK_START' | 'BLOCK_DELTA' | 'BLOCK_STOP' | 'TURN_STOP' | 'ERROR';
  block_id?: number;
  message_group?: number;
  kind?: Block['type'];
  tool_id?: string;
  name?: string;
  text_delta?: string;
  signature_delta?: string;
  thinking_id?: string;
  encrypted_content?: string;
  redacted_content?: string;
  namespace?: string;
  input?: Record<string, unknown>;
  tool_args_delta?: string;
  stop_reason?: string;
  raw_stop_reason?: string;
  usage_in?: number;
  usage_out?: number;
  usage_cache_read?: number;
  usage_cache_write?: number;
  usage_reasoning?: number;
  error_message?: string;
};
export type ProjectionReport = { model: string; boundary: 'request' | 'response'; omitted_reasoning_blocks: number; omitted_opaque_state_blocks: number; omitted_cache_controls?: number };
export type ExecutionStatus = 'running' | 'completed' | 'failed' | 'cancelled';
export type ChildExecution = { session_id: string; turn_id: string; role: string; status: ExecutionStatus };
export type SessionEvent =
  | { kind: 'native_compaction'; payload: { harness: 'codex' | 'claude' } }
  | { kind: 'native_checkpoint'; payload: { scope: string; context_digest: string; prefix_digest: string; harness: 'codex' | 'claude'; native_id: string; claim: string; message_count: number } }
  | { kind: 'replay_proof'; payload: { scope: string; block_digest: string; wire: 'anthropic' | 'responses'; thinking_binding?: string } }
  | { kind: 'child_execution'; payload: ChildExecution }
  | { kind: 'projection'; payload: ProjectionReport }
  | { kind: 'history'; payload: { scope: string; phase: 'message' | 'commit'; index?: number; count?: number; message?: Message } }
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
  nativeStopReason?: string;
  usage?: { input: number; output: number; cacheRead?: number; cacheWrite?: number; reasoning?: number };
};
export type Control = {
  kind: string; request: Record<string, unknown>; response?: Record<string, unknown>;
  requestedBy: string; resolvedBy?: string; turn: string; closed?: boolean;
};
export type SessionState = {
  session: string; sequence: string; messages: ProjectedMessage[];
  controls: Record<string, Control>; activeTurns: string[]; errors: string[];
  projections: (ProjectionReport & { id: string; turn: string })[];
  children: Record<string, ChildExecution>;
  executions: Record<string, 'running' | 'completed' | 'failed' | 'cancelled'>;
};
export function emptySession(session: string): SessionState {
  return { session, sequence: '0', messages: [], controls: {}, activeTurns: [], errors: [], projections: [], executions: {}, children: {} };
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
  if (block?.message_group !== undefined && (!Number.isInteger(block.message_group) || block.message_group < 1 || block.message_group > 4096))
    throw new Error('Invalid message group');
  if (!block || !kinds.has(block.type)) throw new Error('Unsupported canonical block');
  if ((block.type === 'text' || block.type === 'thinking') && typeof block.text !== 'string')
    throw new Error('Text block requires text');
  if (block.thinking_binding !== undefined && (block.type !== 'thinking' || typeof block.thinking_binding !== 'string' ||
      !/^ap1:[0-9a-f]{64}:[0-9a-f]{64}:[0-9a-f]{64}$/.test(block.thinking_binding))) throw new Error('Invalid thinking replay binding');
  if (block.signature !== undefined && typeof block.signature !== 'string')
    throw new Error('Invalid thinking signature');
  for (const field of ['thinking_id', 'encrypted_content', 'redacted_content', 'cache_control', 'cache_ttl'] as const) {
    if (block[field] !== undefined && typeof block[field] !== 'string')
      throw new Error(`Invalid block ${field}`);
  }
  if ((block.type === 'tool_use' || block.type === 'tool_result') && !nonempty(block.tool_id))
    throw new Error('Tool block requires tool_id');
  if (block.type === 'tool_use' && !nonempty(block.name)) throw new Error('Tool use requires name');
  if (block.is_error !== undefined && typeof block.is_error !== 'boolean')
    throw new Error('Invalid tool result status');
  if ((block.type === 'image' || block.type === 'document') &&
      (!nonempty(block.media_ref) || (block.media_type !== undefined && !nonempty(block.media_type)))) throw new Error('Invalid media reference');
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
    case 'native_compaction': {
      if (Object.keys(event.payload).length !== 1 || !['codex', 'claude'].includes(event.payload.harness))
        throw new Error('Invalid native compaction observation');
      return next;
    }
    case 'native_checkpoint': {
      const checkpoint = event.payload;
      if (!Object.keys(checkpoint).every(key => ['scope', 'context_digest', 'prefix_digest', 'harness', 'native_id', 'claim', 'message_count'].includes(key)) ||
          ![checkpoint.scope, checkpoint.context_digest, checkpoint.prefix_digest].every(value => typeof value === 'string' && /^[0-9a-f]{64}$/.test(value)) ||
          typeof checkpoint.claim !== 'string' || !/^[0-9a-f]{32}$/.test(checkpoint.claim) ||
          !['codex', 'claude'].includes(checkpoint.harness) || !nonempty(checkpoint.native_id) || new TextEncoder().encode(checkpoint.native_id).length > 1024 ||
          !Number.isInteger(checkpoint.message_count) || checkpoint.message_count < 1 || checkpoint.message_count > 4096)
        throw new Error('Invalid native cache checkpoint');
      return next; // host cache metadata advances the cursor, never model content
    }
    case 'replay_proof': {
      const proof = event.payload;
      if (typeof proof.scope !== 'string' || typeof proof.block_digest !== 'string' ||
          !Object.keys(proof).every(key => ['scope', 'block_digest', 'wire', 'thinking_binding'].includes(key)) ||
          !/^[0-9a-f]{64}$/.test(proof.scope) || !/^[0-9a-f]{64}$/.test(proof.block_digest) ||
          !(proof.wire === 'responses' ? proof.thinking_binding === undefined :
            proof.wire === 'anthropic' && typeof proof.thinking_binding === 'string' &&
            /^ap1:[0-9a-f]{64}:[0-9a-f]{64}:[0-9a-f]{64}$/.test(proof.thinking_binding)))
        throw new Error('Invalid native replay proof');
      return next; // advance the durable cursor without creating model content
    }
    case 'child_execution': {
      const child = event.payload;
      if (![child.session_id, child.turn_id, child.role].every(value => nonempty(value) && new TextEncoder().encode(value).length <= 127) ||
          child.session_id === state.session || !['running', 'completed', 'failed', 'cancelled'].includes(child.status))
        throw new Error('Invalid child execution reference');
      const key = `${child.session_id}:${child.turn_id}`;
      const previous = state.children?.[key];
      if (previous && (previous.role !== child.role || previous.status !== 'running' && previous.status !== child.status))
        throw new Error('Child execution identity or terminal status changed');
      next.children = { ...(state.children ?? {}), [key]: { ...child } };
      return next;
    }
    case 'projection': {
      const report = event.payload;
      if (!nonempty(report.model) || (report.boundary !== 'request' && report.boundary !== 'response') || !Number.isSafeInteger(report.omitted_reasoning_blocks) || report.omitted_reasoning_blocks < 0 ||
          !Number.isSafeInteger(report.omitted_cache_controls ?? 0) || (report.omitted_cache_controls ?? 0) < 0 ||
          (report.omitted_reasoning_blocks === 0 && (report.omitted_cache_controls ?? 0) === 0) ||
          !Number.isSafeInteger(report.omitted_opaque_state_blocks) || report.omitted_opaque_state_blocks < 0 ||
          report.omitted_opaque_state_blocks > report.omitted_reasoning_blocks) throw new Error('Invalid projection report');
      next.projections = [...(state.projections ?? []), { ...report, id: envelope.event_id, turn: envelope.turn_id }];
      return next;
    }
    case 'history': {
      const history = event.payload;
      if (!nonempty(history.scope) || (history.phase !== 'message' && history.phase !== 'commit'))
        throw new Error('Invalid history checkpoint');
      if (history.phase === 'message') {
        if (!Number.isSafeInteger(history.index ?? 0) || (history.index ?? 0) < 0 || !history.message ||
            !nonempty(history.message.role) || !Array.isArray(history.message.blocks))
          throw new Error('Invalid history checkpoint message');
        if (history.message.content_scalar !== undefined && typeof history.message.content_scalar !== 'boolean') throw new Error('Invalid content form');
        history.message.blocks.forEach(validateBlock);
      } else if (!Number.isSafeInteger(history.count ?? 0) || (history.count ?? 0) < 0)
        throw new Error('Invalid history checkpoint count');
      // Host prompt checkpoints advance the same cursor, without duplicating
      // the immutable observation timeline or exposing child prompts there.
      return next;
    }
    case 'message': {
      if (!nonempty(event.message_id) || !nonempty(event.payload.role) || !Array.isArray(event.payload.blocks) || event.payload.blocks.length > 4096)
        throw new Error('Invalid canonical message');
      if (event.payload.content_scalar !== undefined && typeof event.payload.content_scalar !== 'boolean') throw new Error('Invalid content form');
      event.payload.blocks.forEach(validateBlock);
      const message: ProjectedMessage = { ...event.payload, id: event.message_id,
        turn: envelope.turn_id, complete: true, argumentFragments: {}, stoppedBlocks: event.payload.blocks.map((_, index) => index) };
      const index = state.messages.findIndex(m => m.id === event.message_id);
      if (index >= 0) {
        if (state.messages[index].role !== message.role || state.messages[index].turn !== message.turn)
          throw new Error('Message identity changed');
        const previous = state.messages[index];
        // A complete block snapshot inside a started model turn still awaits
        // TURN_STOP, which owns stop reason and usage. Standalone messages and
        // snapshots after TURN_STOP are already complete.
        message.complete = previous.complete;
        message.usage = previous.usage;
        message.stopReason = previous.stopReason;
        message.nativeStopReason = previous.nativeStopReason;
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
      if (d.message_group !== undefined && (!Number.isInteger(d.message_group) || d.message_group < 1 || d.message_group > 4096 || d.type !== 'BLOCK_START' || d.kind !== 'text')) throw new Error('Invalid authored message group');
      for (const key of ['thinking_id', 'encrypted_content'] as const) {
        if (d[key] !== undefined && (typeof d[key] !== 'string' || d.kind !== 'thinking' ||
          !['BLOCK_START', 'BLOCK_DELTA'].includes(d.type))) throw new Error('Invalid opaque thinking metadata');
      }
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
        for (const value of [d.usage_in, d.usage_out, d.usage_cache_read, d.usage_cache_write, d.usage_reasoning]) {
          if (value !== undefined && (!Number.isSafeInteger(value) || value < 0))
            throw new Error('Invalid model usage');
        }
        message.usage = { input: d.usage_in || 0, output: d.usage_out || 0,
          ...(d.usage_cache_read !== undefined ? { cacheRead: d.usage_cache_read } : {}),
          ...(d.usage_cache_write !== undefined ? { cacheWrite: d.usage_cache_write } : {}),
          ...(d.usage_reasoning !== undefined ? { reasoning: d.usage_reasoning } : {}) };
        message.stopReason = d.stop_reason;
        message.nativeStopReason = d.raw_stop_reason;
        message.complete = true;
        return next;
      }
      const blockID = d.block_id;
      if (blockID === undefined || !Number.isSafeInteger(blockID) || blockID < 0 || blockID > 4095)
        throw new Error('Invalid block identity');
      if (d.type === 'BLOCK_START') {
        if (!d.kind || !['text', 'thinking', 'tool_use'].includes(d.kind) || message.blocks[blockID]) throw new Error('Invalid block start');
        if (d.signature_delta !== undefined && (d.kind !== 'thinking' || typeof d.signature_delta !== 'string')) throw new Error('Invalid thinking signature');
        if (d.redacted_content !== undefined && (d.kind !== 'thinking' || typeof d.redacted_content !== 'string')) throw new Error('Invalid redacted content');
        if (d.input !== undefined && (d.kind !== 'tool_use' || !d.input || typeof d.input !== 'object' || Array.isArray(d.input))) throw new Error('Invalid initial tool input');
        const block: Block = { ...(d.message_group !== undefined ? { message_group: d.message_group } : {}), type: d.kind, tool_id: d.tool_id, name: d.name, namespace: d.namespace,
          ...(d.kind === 'tool_use' ? { input: d.input ?? {} } : { text: d.text_delta ?? '' }),
          ...(d.thinking_id !== undefined ? { thinking_id: d.thinking_id } : {}),
          ...(d.encrypted_content !== undefined ? { encrypted_content: d.encrypted_content } : {}),
          ...(d.signature_delta !== undefined ? { signature: d.signature_delta } : {}), ...(d.redacted_content !== undefined ? { redacted_content: d.redacted_content } : {}) };
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
        for (const key of ['thinking_id', 'encrypted_content'] as const) {
          if (d[key] !== undefined) {
            if (block[key] !== undefined && block[key] !== d[key]) throw new Error('Opaque thinking metadata changed');
            updated[key] = d[key];
          }
        }
        if (d.signature_delta !== undefined) {
          if (typeof d.signature_delta !== 'string' || block.type !== 'thinking') throw new Error('Invalid thinking signature delta');
          updated.signature = (block.signature || '') + d.signature_delta;
        }
        if (d.tool_args_delta !== undefined) {
          if (typeof d.tool_args_delta !== 'string') throw new Error('Invalid argument delta');
          if (block.type !== 'tool_use') throw new Error('Arguments in non-tool block');
          if (d.tool_args_delta && block.input && Object.keys(block.input).length > 0) throw new Error('Arguments after complete initial input');
          message.argumentFragments[blockID] = (message.argumentFragments[blockID] || '') + d.tool_args_delta;
          if (d.tool_args_delta) delete updated.input;
        }
        message.blocks[blockID] = updated;
        return next;
      }
      if (d.type === 'BLOCK_STOP') {
        if (block.type === 'tool_use') message.blocks[blockID] = { ...block,
          input: message.argumentFragments[blockID] ? parseJSON(message.argumentFragments[blockID]) as unknown : block.input ?? {} };
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
/** UI identity belongs to the projection, never to a canonical tool_id. */
export const toolBlockKey = (message: Pick<ProjectedMessage, 'id'>, index: number): string =>
  JSON.stringify([message.id, index]);
export function toolResults(state: SessionState): { byCall: Map<string, Block[]>; matched: Set<string> } {
  type Call = { key: string; turn: string; message: string; resolved: boolean };
  const byCall = new Map<string, Block[]>();
  const matched = new Set<string>();
  const pending = new Map<string, Call[]>();
  const bindings = new Map<string, Call>();
  for (const message of state.messages) for (const [index, block] of message.blocks.entries()) {
    if (!block.tool_id) continue;
    const scope = JSON.stringify([message.turn, block.tool_id]);
    if (block.type === 'tool_use') {
      // A later model response starts a new cycle. Duplicate IDs inside one
      // response stay ambiguous; interleaved execution turns remain separate.
      const previous = (pending.get(block.tool_id) ?? []).filter(call =>
        call.message === message.id || (!call.resolved && call.turn !== message.turn));
      previous.push({ key: toolBlockKey(message, index), turn: message.turn, message: message.id, resolved: false });
      pending.set(block.tool_id, previous);
      bindings.delete(scope);
      continue;
    }
    if (block.type !== 'tool_result') continue;
    let call = bindings.get(scope);
    if (!call) {
      const candidates = pending.get(block.tool_id) ?? [];
      const sameTurn = candidates.filter(candidate => candidate.turn === message.turn);
      const eligible = sameTurn.length ? sameTurn : candidates.filter(candidate =>
        !candidate.resolved);
      if (eligible.length !== 1) continue; // Preserve uncorrelated evidence visibly.
      call = eligible[0];
      bindings.set(scope, call);
    }
    call.resolved = true;
    byCall.set(call.key, [...(byCall.get(call.key) ?? []), block]);
    matched.add(toolBlockKey(message, index));
  }
  return { byCall, matched };
}
