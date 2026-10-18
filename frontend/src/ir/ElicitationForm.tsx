import { useState } from 'react';
type JsonObject = Record<string, unknown>;
type Value = string | number | boolean | string[];
export type FormContent = Record<string, Value>;
const object = (value: unknown): value is JsonObject => !!value && typeof value === 'object' && !Array.isArray(value);
type Option = { value: string; label: string };
function options(schema: JsonObject): Option[] | undefined {
  if (Array.isArray(schema.enum) && schema.enum.every(value => typeof value === 'string')) {
    return schema.enum.map((value, index) => ({ value, label: Array.isArray(schema.enumNames) && typeof schema.enumNames[index] === 'string' ? schema.enumNames[index] : value }));
  }
  const choices = schema.oneOf ?? schema.anyOf;
  if (Array.isArray(choices) && choices.every(item => object(item) && typeof item.const === 'string')) {
    return choices.map(item => ({ value: item.const as string, label: typeof item.title === 'string' ? item.title : item.const as string }));
  }
  return undefined;
}
function supported(schema: JsonObject): boolean {
  if (schema.type === 'array') return object(schema.items) && !!options(schema.items);
  if (!['string', 'boolean', 'number', 'integer'].includes(String(schema.type))) return false;
  if (schema.enum || schema.oneOf || schema.anyOf) return !!options(schema);
  return true;
}
function valid(schema: JsonObject, value: Value | undefined): boolean {
  if (value === undefined) return false;
  if (schema.type === 'boolean') return typeof value === 'boolean';
  if (schema.type === 'number' || schema.type === 'integer') return typeof value === 'number' && Number.isFinite(value) &&
    Math.abs(value) <= Number.MAX_SAFE_INTEGER && (schema.type !== 'integer' || Number.isInteger(value)) &&
    (typeof schema.minimum !== 'number' || value >= schema.minimum) && (typeof schema.maximum !== 'number' || value <= schema.maximum);
  if (schema.type === 'array') {
    const choices = object(schema.items) ? options(schema.items) : undefined;
    return Array.isArray(value) && !!choices && value.every(item => choices.some(choice => choice.value === item)) &&
      (typeof schema.minItems !== 'number' || value.length >= schema.minItems) && (typeof schema.maxItems !== 'number' || value.length <= schema.maxItems);
  }
  const choices = options(schema);
  return typeof value === 'string' && (!choices || choices.some(option => option.value === value)) &&
    (typeof schema.minLength !== 'number' || [...value].length >= schema.minLength) && (typeof schema.maxLength !== 'number' || [...value].length <= schema.maxLength);
}
export default function ElicitationForm({ form, disabled, onSubmit }: { form: JsonObject; disabled: boolean; onSubmit: (content: FormContent) => void }) {
  const schema = object(form.schema) ? form.schema : {};
  const fields = object(schema.properties) ? Object.entries(schema.properties) : [];
  const required = Array.isArray(schema.required) ? schema.required.filter((key): key is string => typeof key === 'string') : [];
  const [values, setValues] = useState<FormContent>(() => Object.fromEntries(fields.flatMap(([key, field]) => {
    if (!object(field)) return [];
    if (valid(field, field.default as Value)) return [[key, field.default as Value]];
    if (field.type === 'boolean') return [[key, false]];
    if (field.type === 'array') return [[key, []]];
    return [];
  })));
  const isSupported = schema.type === 'object' && object(schema.properties) && fields.every(([, field]) => object(field) && supported(field)) &&
    required.every(key => Object.hasOwn(schema.properties as object, key));
  const ready = isSupported && fields.every(([key, field]) => object(field) &&
    (Object.hasOwn(values, key) ? valid(field, values[key]) : !required.includes(key)));
  function set(key: string, value: Value | undefined) {
    setValues(old => { const next = { ...old }; if (value === undefined) delete next[key]; else Object.defineProperty(next, key, { value, writable: true, enumerable: true, configurable: true }); return next; });
  }
  return <form onSubmit={event => { event.preventDefault(); if (ready && !disabled) onSubmit(values); }}>
    {typeof form.message === 'string' && <p>{form.message}</p>}
    {typeof form.server === 'string' && form.server && <p>Requested by {form.server}</p>}
    {!isSupported && <p>This form uses an unsupported field. You can decline the request.</p>}
    {isSupported && fields.map(([key, raw]) => {
      const field = raw as JsonObject;
      const label = typeof field.title === 'string' ? field.title : key;
      const choices = options(field.type === 'array' ? field.items as JsonObject : field);
      const value = Object.hasOwn(values, key) ? values[key] : undefined;
      return <fieldset key={key} disabled={disabled}>
        <legend>{label}{required.includes(key) ? ' (required)' : ''}</legend>
        {typeof field.description === 'string' && <p>{field.description}</p>}
        {field.type === 'boolean' ? <label><input type="checkbox" checked={value === true} onChange={event => set(key, event.target.checked)} />{label}</label> :
          field.type === 'array' && choices ? choices.map(option => <label key={option.value} style={{ display: 'block' }}><input type="checkbox" checked={Array.isArray(value) && value.includes(option.value)}
            onChange={event => set(key, event.target.checked ? [...(Array.isArray(value) ? value : []), option.value] : (Array.isArray(value) ? value : []).filter(item => item !== option.value))} />{option.label}</label>) :
          choices ? <label>{label}<select value={choices.findIndex(option => option.value === value)} onChange={event => set(key, choices[Number(event.target.value)]?.value)} required={required.includes(key)}>
            <option value={-1}>Choose…</option>{choices.map((option, index) => <option key={option.value} value={index}>{option.label}</option>)}
          </select></label> : <label>{label}<input aria-label={label} value={typeof value === 'string' || typeof value === 'number' ? value : ''}
            type={field.type === 'number' || field.type === 'integer' ? 'number' : field.format === 'email' ? 'email' : field.format === 'uri' ? 'url' : field.format === 'date' ? 'date' : 'text'}
            step={field.type === 'integer' ? 1 : 'any'} min={typeof field.minimum === 'number' ? field.minimum : undefined} max={typeof field.maximum === 'number' ? field.maximum : undefined}
            required={required.includes(key) && (field.type !== 'string' || (typeof field.minLength === 'number' && field.minLength > 0))} onChange={event => set(key, field.type === 'number' || field.type === 'integer' ? event.target.value === '' ? undefined : event.target.valueAsNumber : event.target.value)} /></label>}
      </fieldset>;
    })}
    {isSupported && <button type="submit" disabled={disabled || !ready}>Send form</button>}
  </form>;
}
