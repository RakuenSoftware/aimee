// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import MemoryBackend from './MemoryBackend';
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
 it('offers only the supported backend and explicitly saves native', async () => {
  const stub=transport(), onSaved=vi.fn();render(<MemoryBackend config={{}} onSaved={onSaved} fetchImpl={stub.fetchImpl}/>);
  expect(screen.getAllByRole('option').map(x=>x.textContent)).toEqual(['Aimee native memory']);
  fireEvent.click(screen.getByText('Save memory backend'));
  await waitFor(()=>expect(onSaved).toHaveBeenCalledWith(['memory.backend']));
  expect(stub.calls).toEqual([{key:'memory.backend',value:{engine:'native',endpoint:'',auth:'bearer'}}]);
 });
 it.each(['cognee','hillock','unknown'])('keeps retired %s visible and requires an explicit replacement', async (engine) => {
  const stub=transport(), onSaved=vi.fn();render(<MemoryBackend config={{memory_backend_engine:engine,memory_backend_token:true}} onSaved={onSaved} fetchImpl={stub.fetchImpl}/>);
  expect((screen.getByLabelText('Memory backend') as HTMLSelectElement).value).toBe(engine);
  expect(screen.getByRole('alert').textContent).toContain('Export and migrate');
  const button=screen.getByText('Save memory backend') as HTMLButtonElement;
  expect(button.disabled).toBe(true); fireEvent.click(button); expect(stub.calls).toHaveLength(0);
  expect(screen.queryByLabelText('Memory service credential')).toBeNull();
  fireEvent.change(screen.getByLabelText('Memory backend'),{target:{value:'native'}});
  fireEvent.click(button); await waitFor(()=>expect(onSaved).toHaveBeenCalledOnce());
  expect(stub.calls).toEqual([{key:'memory.backend',value:{engine:'native',endpoint:'',auth:'bearer'}}]);
 });
 it('reports a refused replacement without claiming it was saved', async () => {
  const stub=transport('memory.backend'), onSaved=vi.fn();render(<MemoryBackend config={{}} onSaved={onSaved} fetchImpl={stub.fetchImpl}/>);
  fireEvent.click(screen.getByText('Save memory backend'));
  await screen.findByRole('alert');expect(stub.calls).toHaveLength(1);expect(onSaved).not.toHaveBeenCalled();
 });
});
