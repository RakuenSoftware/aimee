// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import MemoryBackend, { validateMemoryEndpoint } from './MemoryBackend';
vi.mock('@rakuensoftware/smoothgui', () => ({ Button: ({ children, ...props }: React.ButtonHTMLAttributes<HTMLButtonElement>) => <button {...props}>{children}</button> }));
afterEach(cleanup);
function transport(failureKey = '') {
 const calls: Array<{key: string; value: unknown}> = [];
 const fetchImpl = vi.fn(async (_url: unknown, request?: RequestInit) => {
  const body = JSON.parse(String(request?.body)); calls.push(body);
  return { status: body.key === failureKey ? 400 : 200, json: async () => body.key === failureKey ? {error:'save refused'} : {value: body.key === 'memory_backend_token' ? true : body.value} } as Response;
 });
 return {calls, fetchImpl:fetchImpl as unknown as typeof fetch};
}
describe('first-start memory selection', () => {
 it('offers all three backends and explicitly saves native', async () => {
  const stub=transport(), onSaved=vi.fn();render(<MemoryBackend config={{}} onSaved={onSaved} fetchImpl={stub.fetchImpl}/>);
  expect(screen.getAllByRole('option').map(x=>x.textContent)).toEqual(['Aimee native memory','Cognee','Hillock']);
  fireEvent.click(screen.getByText('Save memory backend'));
  await waitFor(()=>expect(onSaved).toHaveBeenCalledWith(['memory.backend']));
  expect(stub.calls).toEqual([{key:'memory.backend',value:{engine:'native',endpoint:'',auth:'bearer'}}]);
 });
 it('seals external credentials before saving one complete selection', async () => {
  const stub=transport(), onSaved=vi.fn();render(<MemoryBackend config={{}} onSaved={onSaved} fetchImpl={stub.fetchImpl}/>);
  fireEvent.change(screen.getByLabelText('Memory backend'),{target:{value:'hillock'}});
  fireEvent.change(screen.getByLabelText('Memory service origin'),{target:{value:'http://hillock:8097'}});
  fireEvent.change(screen.getByLabelText('Memory service credential'),{target:{value:'fixture-token'}});
  fireEvent.click(screen.getByText('Save memory backend'));
  await waitFor(()=>expect(onSaved).toHaveBeenCalledOnce());
  expect(stub.calls).toEqual([{key:'memory_backend_token',value:'fixture-token'}, {key:'memory.backend',value:{engine:'hillock',endpoint:'http://hillock:8097',auth:'bearer'}}]);
  expect(screen.getByLabelText('Memory service credential').getAttribute('value')).toBe('');
 });
 it('keeps the old backend selected when credential storage fails', async () => {
  const stub=transport('memory_backend_token'), onSaved=vi.fn();render(<MemoryBackend config={{}} onSaved={onSaved} fetchImpl={stub.fetchImpl}/>);
  fireEvent.change(screen.getByLabelText('Memory backend'),{target:{value:'cognee'}});
  fireEvent.change(screen.getByLabelText('Memory service origin'),{target:{value:'http://cognee:8000'}});
  fireEvent.change(screen.getByLabelText('Memory service authentication'),{target:{value:'api-key'}});
  fireEvent.change(screen.getByLabelText('Memory service credential'),{target:{value:'fixture-token'}});
  fireEvent.click(screen.getByText('Save memory backend'));
  await screen.findByRole('alert');expect(stub.calls).toHaveLength(1);expect(onSaved).not.toHaveBeenCalled();
 });
 it('rejects credentials in URLs and invalid origins', () => {
  for(const origin of ['http://localhost:8097','https://hillock.example','http://[::1]:8097/']) expect(validateMemoryEndpoint(origin)).toBe(true);
  for(const origin of ['','file:///tmp','http://user:token@localhost','http://localhost/path','http://localhost?token=x','http://localhost#fragment']) expect(validateMemoryEndpoint(origin)).toBe(false);
 });
});
