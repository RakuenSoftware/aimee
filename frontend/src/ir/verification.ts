/** Host verification metadata. Admission checks shape, never correctness or
 * permission to apply a lesson. It is not model-visible conversation content. */
export type VerificationStep = {
  name: string; command_sha256: string; ran: boolean; skipped: boolean;
  termination: 'exited'; exit_code: string;
};
export type VerificationProof = {
  version: 1; session_id: string; turn_id: string; actor: string;
  scope_kind: 'project'; scope_id: string; isolation: 'private_source_snapshot';
  snapshot_id: string; snapshot_commit: string; final_commit: string;
  snapshot_tree: string; final_tree: string; configuration_sha256: string;
  final_configuration_sha256: string; profile_sha256: string; completed: boolean;
  observed_at: string; steps: VerificationStep[];
};
const proofKeys = ['version', 'session_id', 'turn_id', 'actor', 'scope_kind', 'scope_id', 'isolation',
  'snapshot_id', 'snapshot_commit', 'final_commit', 'snapshot_tree', 'final_tree',
  'configuration_sha256', 'final_configuration_sha256', 'profile_sha256', 'completed', 'observed_at', 'steps'];
const stepKeys = ['name', 'command_sha256', 'ran', 'skipped', 'termination', 'exit_code'];
function object(value: unknown, keys: string[]): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === keys.length && Object.keys(value).every(key => keys.includes(key));
}
function text(value: unknown, max: number): value is string {
  return typeof value === 'string' && value.length > 0 && !value.includes('\0') && new TextEncoder().encode(value).length <= max;
}
function hex(value: unknown, sizes: number[]): boolean {
  return typeof value === 'string' && sizes.includes(value.length) && /^[0-9a-f]+$/.test(value);
}
export function validVerificationProof(value: unknown): value is VerificationProof {
  if (!object(value, proofKeys) || value.version !== 1 ||
      !text(value.session_id, 256) || !text(value.turn_id, 256) || !text(value.actor, 256) ||
      value.scope_kind !== 'project' || !text(value.scope_id, 256) || value.isolation !== 'private_source_snapshot' ||
      !hex(value.snapshot_id, [32]) || !hex(value.snapshot_commit, [40, 64]) || !hex(value.final_commit, [40, 64]) ||
      !hex(value.snapshot_tree, [40, 64]) || !hex(value.final_tree, [40, 64]) ||
      !hex(value.configuration_sha256, [64]) || !hex(value.final_configuration_sha256, [64]) ||
      !hex(value.profile_sha256, [64]) || typeof value.completed !== 'boolean' || !text(value.observed_at, 64) ||
      !Array.isArray(value.steps) || value.steps.length < 1 || value.steps.length > 64) return false;
  const names = new Set<string>();
  for (const step of value.steps) {
    if (!object(step, stepKeys) || !text(step.name, 128) || names.has(step.name) || !hex(step.command_sha256, [64]) ||
        typeof step.ran !== 'boolean' || typeof step.skipped !== 'boolean' || step.termination !== 'exited' ||
        typeof step.exit_code !== 'string' || !/^(0|[1-9][0-9]{0,2})$/.test(step.exit_code) || Number(step.exit_code) > 255) return false;
    names.add(step.name);
  }
  return new TextEncoder().encode(JSON.stringify(value)).length <= 65536;
}
