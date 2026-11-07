import type { NativeOwner, ToolInterruption } from './interruption';
export const receipt = (): ToolInterruption => ({ version: 1,
  source: { session_id: 's', turn_id: 't', sequence: '2', event_id: 'e2', actor: 'operator',
    message_id: 'm', block_index: 0, tool_id: 'call', name: 'write_file' },
  owner: { owner_id: 'a'.repeat(32), binding_sha256: 'b'.repeat(64), epoch: '0', containment_sha256: 'c'.repeat(64) },
  terminal_sequence: '4', outcome: 'unknown', retry: 'explicit_permission_required' });

export const nativeOwner = (): NativeOwner => ({ version: 1, harness: 'codex', ...(receipt().owner as Omit<NativeOwner, 'version' | 'harness'>) });
