import { canonicalRemote, type CloneKbAnnotations, type GitProjectsResponse, type OwnerRef } from './ownerUrl';

export interface CloneRepo {
  name: string;
  clone_url: string;
}

export interface CloneResult extends CloneKbAnnotations {
  name: string;
  ok: boolean;
  project?: string | null;
  error?: string | null;
}

export interface CloneProgress {
  name: string;
  current: number;
  total: number;
  phase: 'cloning' | 'checking';
}

type Request = (path: string, init?: RequestInit) => Promise<Response>;
type Refresh = () => Promise<GitProjectsResponse | void>;
interface Options {
  onProgress?: (progress: CloneProgress | null) => void;
  recoveryAttempts?: number;
  recoveryDelayMs?: number;
}

/** Give each repo its own deadline. A lost response is reconciled against the
 * published inventory before the queue advances; it never resubmits that clone. */
export async function cloneRepos(
  owner: OwnerRef,
  repos: CloneRepo[],
  request: Request,
  onResult: (result: CloneResult) => void,
  refreshProjects: Refresh,
  { onProgress, recoveryAttempts = 30, recoveryDelayMs = 2000 }: Options = {},
): Promise<string | null> {
  try {
    for (const [index, repo] of repos.entries()) {
      const progress = { name: repo.name, current: index + 1, total: repos.length };
      onProgress?.({ ...progress, phase: 'cloning' });
      let result: CloneResult | undefined;
      let error: string | undefined;
      try {
        const response = await request('/api/git/clone-org', {
          method: 'POST',
          body: JSON.stringify({ ...owner, repos: [repo] }),
        });
        const data = await response.json();
        if (!response.ok) {
          if (![502, 503, 504].includes(response.status)) {
            error = typeof data.error === 'string' ? data.error : `Clone request failed (${response.status}).`;
          }
        } else if (Array.isArray(data.results) && data.results.length === 1 &&
            data.results[0]?.name === repo.name && typeof data.results[0].ok === 'boolean') {
          result = data.results[0];
        }
      } catch {
        // A transport/parse failure says nothing about whether git completed.
      }
      if (result) {
        onResult(result);
        await refreshProjects();
        continue;
      }
      if (error) {
        await refreshProjects();
        return error;
      }
      onProgress?.({ ...progress, phase: 'checking' });
      const remote = canonicalRemote(repo.clone_url);
      for (let attempt = 0; attempt < recoveryAttempts; attempt++) {
        if (attempt > 0) await new Promise(resolve => setTimeout(resolve, recoveryDelayMs));
        const inventory = await refreshProjects();
        const published = remote && inventory?.details?.find(project =>
          project.remote && canonicalRemote(project.remote) === remote);
        if (published) {
          result = { name: repo.name, ok: true, project: published.ref };
          break;
        }
      }
      if (!result) {
        return `Could not confirm the clone of ${repo.name}. The remaining queue is paused. Completed repositories will continue to appear in the project list.`;
      }
      onResult(result);
    }
    return null;
  } finally {
    onProgress?.(null);
  }
}
