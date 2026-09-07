/** @vitest-environment jsdom */
import {cleanup, fireEvent, render, screen, waitFor} from '@testing-library/react';
import {afterEach, describe, expect, it, vi} from 'vitest';
import PrimaryChooser from './PrimaryChooser';
vi.mock('@rakuensoftware/smoothgui', () => ({
  Button: ({children, variant: _variant, ...props}: Record<string, unknown>) => <button {...props}>{children as never}</button>,
}));
afterEach(() => {cleanup(); vi.unstubAllGlobals();});
describe('local primary model', () => {
  it('registers a local OpenAI-compatible model without inventing an API key', async () => {
    const configured = vi.fn();
    const fetch = vi.fn().mockResolvedValue({status:200, json:async () => ({status:'ok'})});
    vi.stubGlobal('fetch', fetch);
    render(<PrimaryChooser onConfigured={configured} />);
    fireEvent.click(screen.getByRole('button', {name:/OpenAI-compatible or local/}));
    fireEvent.change(screen.getByLabelText('Endpoint'), {target:{value:'https://aimee-llm:8761'}});
    fireEvent.change(screen.getByLabelText('Model'), {target:{value:'local-served-model'}});
    fireEvent.click(screen.getByRole('button', {name:'Save & set as primary'}));
    await waitFor(() => expect(configured).toHaveBeenCalledWith('openai'));
    const body = JSON.parse(fetch.mock.calls[0][1].body);
    expect(body.args).toContain('https://aimee-llm:8761');
    expect(body.args).toContain('--default');
    expect(body.args).not.toContain('--key');
  });
  it('still requires a credential for the Anthropic API', () => {
    const fetch = vi.fn(); vi.stubGlobal('fetch',fetch);
    render(<PrimaryChooser onConfigured={vi.fn()} />);
    fireEvent.click(screen.getByRole('button', {name:/Anthropic API/}));
    fireEvent.click(screen.getByRole('button', {name:'Save & set as primary'}));
    expect(screen.getByText('Endpoint, model, and API key are all required.')).toBeTruthy();
    expect(fetch).not.toHaveBeenCalled();
  });
});
