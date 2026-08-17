export function escHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

const ipv4Octet = '(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)';
const machineTokenRe = new RegExp(
  `(^|[^A-Za-z0-9./-])(${ipv4Octet}(?:\\.${ipv4Octet}){3}(?::\\d{1,5})?)(?=$|[^A-Za-z0-9./-])`,
  'g'
);

function renderInline(s: string): string {
  s = protectMachineTokens(s);
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  s = s.replace(/\*(?!\*)([^*]+)\*(?!\*)/g, '<em>$1</em>');
  s = s.replace(/_([^_]+)_/g, '<em>$1</em>');
  s = s.replace(/`([^`]+)`/g, '<code style="background:var(--sg-success-bg);padding:1px 5px;border-radius:3px;font-family:monospace;font-size:0.9em;color:var(--sg-success)">$1</code>');
  s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^)]+)\)/g, '<a href="$2" target="_blank" rel="noopener noreferrer" style="color:var(--sg-primary)">$1</a>');
  return s;
}

function protectMachineTokens(s: string): string {
  return s.replace(machineTokenRe, (_match, prefix: string, token: string) =>
    `${prefix}<span style="white-space:nowrap">${token}</span>`);
}

export function renderMd(text: string): string {
  const lines = text.split('\n');
  const out: string[] = [];
  let inCode = false;
  let codeLang = '';
  let codeLines: string[] = [];
  let inList = false;
  let inOrderedList = false;
  let listItems: string[] = [];

  const flushList = () => {
    if (inList) {
      out.push(`<ul style="margin:6px 0 6px 18px;padding:0">${listItems.map(li => `<li style="margin:2px 0">${li}</li>`).join('')}</ul>`);
      listItems = [];
      inList = false;
    }
    if (inOrderedList) {
      out.push(`<ol style="margin:6px 0 6px 18px;padding:0">${listItems.map(li => `<li style="margin:2px 0">${li}</li>`).join('')}</ol>`);
      listItems = [];
      inOrderedList = false;
    }
  };

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];

    if (inCode) {
      if (/^(`{3,}|~{3,})\s*$/.test(raw)) {
        const langAttr = codeLang ? ` class="language-${escHtml(codeLang)}"` : '';
        const codeContent = codeLines.map(l => escHtml(l)).join('\n');
        out.push(
          `<div style="margin:8px 0;border-radius:6px;overflow:hidden;border:1px solid var(--sg-text)">` +
          `<div style="background:var(--sg-text);padding:4px 10px;font-size:11px;color:var(--sg-text-secondary);display:flex;justify-content:space-between">` +
          `<span>${codeLang ? escHtml(codeLang) : 'code'}</span></div>` +
          `<pre style="margin:0;padding:10px 12px;background:var(--sg-text);overflow-x:auto;font-size:13px;line-height:1.4">` +
          `<code${langAttr} style="font-family:monospace">${codeContent}</code></pre></div>`
        );
        inCode = false;
        codeLang = '';
        codeLines = [];
      } else {
        codeLines.push(raw);
      }
      continue;
    }

    const fenceMatch = raw.match(/^(`{3,}|~{3,})(\w*)/);
    if (fenceMatch) {
      flushList();
      inCode = true;
      codeLang = fenceMatch[2] ?? '';
      codeLines = [];
      continue;
    }

    const line = escHtml(raw);
    const h1 = line.match(/^#\s+(.+)/);
    const h2 = line.match(/^##\s+(.+)/);
    const h3 = line.match(/^###\s+(.+)/);
    if (h3) { flushList(); out.push(`<h3 style="margin:10px 0 4px;font-size:15px;color:var(--sg-primary)">${renderInline(h3[1])}</h3>`); continue; }
    if (h2) { flushList(); out.push(`<h2 style="margin:12px 0 4px;font-size:17px;color:var(--sg-primary)">${renderInline(h2[1])}</h2>`); continue; }
    if (h1) { flushList(); out.push(`<h1 style="margin:12px 0 6px;font-size:20px;color:var(--sg-primary)">${renderInline(h1[1])}</h1>`); continue; }

    if (/^(-{3,}|\*{3,}|={3,})$/.test(raw)) {
      flushList();
      out.push('<hr style="border:none;border-top:1px solid var(--sg-text);margin:12px 0"/>');
      continue;
    }

    const bq = line.match(/^&gt;\s?(.*)/);
    if (bq) {
      flushList();
      out.push(`<blockquote style="border-left:3px solid var(--sg-text-muted);margin:4px 0;padding:4px 12px;color:var(--sg-text-pale);font-style:italic">${renderInline(bq[1])}</blockquote>`);
      continue;
    }

    const ulMatch = line.match(/^[-*+]\s+(.+)/);
    if (ulMatch) {
      flushList();
      if (!inList) inList = true;
      listItems.push(renderInline(ulMatch[1]));
      const nextRaw = lines[i + 1];
      if (!nextRaw || !nextRaw.match(/^[-*+]\s+/)) flushList();
      continue;
    }

    const olMatch = line.match(/^\d+\.\s+(.+)/);
    if (olMatch) {
      if (!inOrderedList) inOrderedList = true;
      listItems.push(renderInline(olMatch[1]));
      const nextRaw = lines[i + 1];
      if (!nextRaw || !nextRaw.match(/^\d+\.\s+/)) flushList();
      continue;
    }

    if (raw.trim() === '') {
      flushList();
      out.push('<div style="height:6px"></div>');
      continue;
    }

    flushList();
    out.push(`<p style="margin:2px 0">${renderInline(line)}</p>`);
  }

  if (inCode && codeLines.length > 0) {
    const codeContent = codeLines.map(l => escHtml(l)).join('\n');
    const langAttr = codeLang ? ` class="language-${escHtml(codeLang)}"` : '';
    out.push(
      `<div style="margin:8px 0;border-radius:6px;overflow:hidden;border:1px solid var(--sg-text)">` +
      `<div style="background:var(--sg-text);padding:4px 10px;font-size:11px;color:var(--sg-text-secondary)">` +
      `<span>${codeLang ? escHtml(codeLang) : 'code'}</span></div>` +
      `<pre style="margin:0;padding:10px 12px;background:var(--sg-text);overflow-x:auto;font-size:13px;line-height:1.4">` +
      `<code${langAttr} style="font-family:monospace">${codeContent}</code></pre></div>`
    );
  }

  flushList();
  return out.join('\n');
}

export function renderWithMentions(text: string): string {
  return escHtml(text).replace(/@([A-Za-z0-9._-]+)/g,
    '<span style="color:var(--sg-info-border);font-weight:600">@$1</span>');
}
