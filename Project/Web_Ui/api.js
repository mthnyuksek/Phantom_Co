// Talks to the Node. The session cookie is HttpOnly, so the page never sees it.
// Sends a File/Blob as the raw request body (file upload).
export async function upload(url, file) {
  const r = await fetch(url, { method: 'PUT', credentials: 'same-origin', headers: { 'X-PC2': '1', 'Content-Type': 'application/octet-stream' }, body: file });
  const j = await r.json().catch(() => ({}));
  if (r.status === 401) window.dispatchEvent(new Event('pc2:signedout'));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

export async function api(method, url, body) {
  const r = await fetch(url, {
    method,
    credentials: 'same-origin',
    headers: { 'X-PC2': '1', ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) },
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  const j = await r.json().catch(() => ({}));
  if (r.status === 401 && !url.startsWith('/api/auth/')) window.dispatchEvent(new Event('pc2:signedout'));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
