import type { Message } from './session';
import { stringifyJSON } from './json';

export type ImportReport = {
  version: 1;
  source_format: 'canonical' | 'anthropic' | 'openai_chat' | 'responses' | 'tmux_text' | 'unknown';
  outcome: 'exact' | 'normalized' | 'archived' | 'refused';
  source_sha256: string; source_session: string; source_agent: string; source_provider: string;
  source_messages: number; imported_messages: number; removed_thinking_bindings: number;
  tool_structure: 'preserved' | 'unrecoverable'; archive_chunks: number;
};
export type ImportedEntry = { message: Message } | { parts: string[]; sha256: string; count: number };
export type ImportCheckpoint = { entries: ImportedEntry[]; committed: boolean; bytes: number };
export type ImportObservation = { id: string; turn: string; actor: string; scope: string; report: ImportReport; chunks?: string[]; checkpoint?: ImportCheckpoint };
export type ImportHistory =
  | { scope: string; phase: 'message_fragment'; fragment: { message_index: number; part_index: number; part_count: number; message_sha256: string; text: string } }
  | { scope: string; phase: 'import_report'; import_report: ImportReport }
  | { scope: 'legacy-archive'; phase: 'archive'; index: number; archive_text: string };
const fields = ['version', 'source_format', 'outcome', 'source_sha256', 'source_session', 'source_agent', 'source_provider',
  'source_messages', 'imported_messages', 'removed_thinking_bindings', 'tool_structure', 'archive_chunks'];
const object = (value: unknown, keys: string[]): value is Record<string, unknown> => !!value && typeof value === 'object' && !Array.isArray(value) &&
  Object.keys(value).length === keys.length && Object.keys(value).every(key => keys.includes(key));
function text(value: unknown, minimum: number, maximum: number): value is string {
  if (typeof value !== 'string') return false;
  try { stringifyJSON(value); } catch { return false; }
  const bytes = new TextEncoder().encode(value).length;
  return bytes >= minimum && bytes <= maximum;
}
const count = (value: unknown, maximum: number): value is number => typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= maximum;
export function validImportReport(value: unknown): value is ImportReport {
  if (!object(value, fields) || value.version !== 1 ||
      !['canonical', 'anthropic', 'openai_chat', 'responses', 'tmux_text', 'unknown'].includes(value.source_format as string) ||
      !['exact', 'normalized', 'archived', 'refused'].includes(value.outcome as string) ||
      typeof value.source_sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(value.source_sha256) ||
      !text(value.source_session, 1, 127) || !text(value.source_agent, 0, 63) || !text(value.source_provider, 0, 63) ||
      !count(value.source_messages, 16777216) || !count(value.imported_messages, 16777216) || !count(value.removed_thinking_bindings, 16777216) ||
      !count(value.archive_chunks, 4096)) return false;
  if (value.outcome === 'archived') return value.source_format === 'tmux_text' && value.source_messages === 0 &&
    value.imported_messages === 0 && value.removed_thinking_bindings === 0 && value.tool_structure === 'unrecoverable' && value.archive_chunks > 0;
  if (value.archive_chunks !== 0) return false;
  if (value.outcome === 'refused') return value.imported_messages === 0 && value.tool_structure === 'unrecoverable';
  if (value.tool_structure !== 'preserved') return false;
  return value.outcome === 'exact' ? value.source_format === 'canonical' && value.source_messages === value.imported_messages && value.removed_thinking_bindings === 0 :
    ['canonical', 'anthropic', 'openai_chat', 'responses'].includes(value.source_format as string);
}
export function validImportHistory(value: unknown): value is ImportHistory {
  if (!value || typeof value !== 'object') return false;
  const payload = value as Record<string, unknown>;
  if (payload.phase === 'message_fragment') {
    if (!object(value, ['scope', 'phase', 'fragment']) || !text(payload.scope, 1, 128) ||
        !object(payload.fragment, ['message_index', 'part_index', 'part_count', 'message_sha256', 'text'])) return false;
    const fragment = payload.fragment;
    return count(fragment.message_index, 16777215) && count(fragment.part_index, 4095) && count(fragment.part_count, 4096) &&
      fragment.part_count > 0 && fragment.part_index < fragment.part_count && typeof fragment.message_sha256 === 'string' &&
      /^[a-f0-9]{64}$/.test(fragment.message_sha256) && text(fragment.text, 1, 65536);
  }
  if (payload.phase === 'archive') return object(value, ['scope', 'phase', 'index', 'archive_text']) && payload.scope === 'legacy-archive' &&
    count(payload.index, 4095) && text(payload.archive_text, 1, 65536);
  return payload.phase === 'import_report' && object(value, ['scope', 'phase', 'import_report']) && text(payload.scope, 1, 128) &&
    validImportReport(payload.import_report) && (payload.import_report.outcome !== 'archived' || payload.scope === 'legacy-archive');
}
