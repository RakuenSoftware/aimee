export function validateMemoryEndpoint(endpoint: string): boolean {
  try {
    const url = new URL(endpoint);
    return endpoint.length <= 2048 && ['http:', 'https:'].includes(url.protocol) && !!url.hostname && !url.username && !url.password &&
      (!url.pathname || url.pathname === '/') && !url.search && !url.hash && !/[\s\\]/.test(endpoint);
  } catch { return false; }
}

