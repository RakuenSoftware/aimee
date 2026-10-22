export interface ChannelMessage {
  id?: string;
  sender: string;
  text: string;
  created_at?: string;
}

// IDs are decimal strings so SQLite's full integer range stays exact in JS.
export function mergeChannelMessages(existing: ChannelMessage[], incoming: ChannelMessage[]): ChannelMessage[] {
  const ids = new Set<string>();
  const messages = [...existing, ...incoming].filter(message => {
    if (!message.id || !/^[1-9]\d*$/.test(message.id)) return true;
    if (ids.has(message.id)) return false;
    ids.add(message.id);
    return true;
  });
  if (messages.every(message => message.id && /^[1-9]\d*$/.test(message.id))) {
    messages.sort((a, b) => a.id!.length - b.id!.length || a.id!.localeCompare(b.id!));
  }
  return messages;
}
