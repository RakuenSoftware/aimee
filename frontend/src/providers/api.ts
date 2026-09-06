export interface ProviderConnection {
  name: string;
  provider: string;
  endpoint: string;
  auth_type: string;
  model_count: number;
}

export async function providerRequest<T>(path: string, body?: unknown): Promise<T> {
  const response = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    headers: { "Content-Type": "application/json", "X-CSRF-Token": window._csrf || "" },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
  });
  const data = await response.json();
  if (!response.ok || data.error || (data.status && data.status !== "ok")) {
    throw new Error(data.error || data.message || `Request failed (${response.status})`);
  }
  return data as T;
}
