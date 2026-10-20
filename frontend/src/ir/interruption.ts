/** A durable host lifecycle observation. This is neither a tool execution
 * result nor permission to retry an effect whose completion was lost. */
export type ToolInterruption = {
  version: 1;
  source: { session_id: string; turn_id: string; sequence: string; event_id: string;
    actor: string; message_id: string; block_index: number; tool_id: string; name: string };
  owner: { owner_id: string; binding_sha256: string; epoch: string; containment_sha256: string };
  terminal_sequence: string; outcome: 'unknown'; retry: 'explicit_permission_required';
};
export const toolInterruptionText = "The host lost this tool's completion result. Its effects are unknown; interruption does not prove that it failed or was cancelled. Do not repeat an effect without explicit user authorization.";
function object(value: unknown, keys: string[]): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === keys.length && Object.keys(value).every(key => keys.includes(key));
}
function text(value: unknown, max: number): value is string {
  return typeof value === 'string' && value.length > 0 && !value.includes('\0') &&
    new TextEncoder().encode(value).length <= max;
}
function positiveSequence(value: unknown): value is string {
  return typeof value === 'string' && /^[1-9][0-9]{0,18}$/.test(value) && BigInt(value) <= 9223372036854775807n;
}
function hex(value: unknown, size: number): boolean {
  return typeof value === 'string' && value.length === size && /^[0-9a-f]+$/.test(value);
}
export function validToolInterruption(value: unknown): value is ToolInterruption {
  if (!object(value, ['version', 'source', 'owner', 'terminal_sequence', 'outcome', 'retry']) ||
      value.version !== 1 || value.outcome !== 'unknown' || value.retry !== 'explicit_permission_required' ||
      !positiveSequence(value.terminal_sequence) ||
      !object(value.source, ['session_id', 'turn_id', 'sequence', 'event_id', 'actor', 'message_id', 'block_index', 'tool_id', 'name']) ||
      !object(value.owner, ['owner_id', 'binding_sha256', 'epoch', 'containment_sha256'])) return false;
  const source = value.source, owner = value.owner;
  if (!['session_id', 'turn_id', 'actor'].every(key => text(source[key], 128)) ||
      !['event_id', 'message_id', 'tool_id', 'name'].every(key => text(source[key], 1024)) ||
      !positiveSequence(source.sequence) || !Number.isInteger(source.block_index) ||
      typeof source.block_index !== 'number' || source.block_index < 0 || source.block_index > 4095 ||
      !hex(owner.owner_id, 32) || !hex(owner.binding_sha256, 64) || !hex(owner.containment_sha256, 64) ||
      !(owner.epoch === '0' || hex(owner.epoch, 32))) return false;
  return new TextEncoder().encode(JSON.stringify(value)).length <= 8192;
}

/** Custody of a canonical closed-block snapshot. Browser admission authenticates
 * no process boundary; it only fences later lifecycle annotations to the same
 * durable host metadata. */
export type NativeOwner = { version: 1; harness: 'codex' | 'claude'; owner_id: string;
  binding_sha256: string; epoch: string; containment_sha256: string };
export function validNativeOwner(value: unknown): value is NativeOwner {
  return object(value, ['version', 'harness', 'owner_id', 'binding_sha256', 'epoch', 'containment_sha256']) &&
    value.version === 1 && (value.harness === 'codex' || value.harness === 'claude') &&
    hex(value.owner_id, 32) && hex(value.binding_sha256, 64) && hex(value.containment_sha256, 64) &&
    (value.epoch === '0' || hex(value.epoch, 32));
}
