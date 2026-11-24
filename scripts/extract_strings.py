#!/usr/bin/env python3
"""Collect the interface texts (TX("...") in src/app) into lang/messages.pot,
and update each lang/<code>.po: new texts added with an empty translation,
texts no longer used kept at the end as comments. The French text is the key
(app/i18n.h)."""

import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIT = r'"(?:[^"\\\n]|\\.)*"'
CALL = re.compile(r'\bTX(?:RL|R|L|F)?\(\s*(' + LIT + r'(?:\s*' + LIT + r')*)\s*[,)]')


def c_unescape(body: str) -> str:
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body):
            e = body[i + 1]
            if e == 'x':
                j = i + 2
                while j < len(body) and j < i + 4 and body[j] in '0123456789abcdefABCDEF':
                    j += 1
                out.append(int(body[i + 2:j], 16))
                i = j
                continue
            out += {'n': b'\n', 't': b'\t', '"': b'"', '\\': b'\\', "'": b"'"}.get(e, e.encode())
            i += 2
            continue
        out += c.encode('utf-8')
        i += 1
    return out.decode('utf-8')


def po_quote(text: str) -> str:
    body = text.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t')
    lines = body.split('\n')
    if len(lines) == 1:
        return '"' + body + '"'
    parts = [l + '\\n' for l in lines[:-1]] + ([lines[-1]] if lines[-1] else [])
    return '""\n' + '\n'.join('"' + p + '"' for p in parts)


def collect():
    keys = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'src', 'app', '*.cpp'))):
        src = open(path, encoding='utf-8').read()
        for m in CALL.finditer(src):
            text = ''.join(c_unescape(p[1:-1]) for p in re.findall(LIT, m.group(1)))
            text = text.split('##')[0]
            if text and text not in keys:
                keys.append(text)
    return keys


def parse_po(text):
    entries, cur, field = {}, {'id': '', 'str': ''}, None
    def flush():
        if field == 'str':
            entries[cur['id']] = cur['str']
    for raw in text.split('\n'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        m = re.match(r'(msgid|msgstr)\s+(".*")$', line)
        if m:
            if m.group(1) == 'msgid':
                flush()
                cur, field = {'id': '', 'str': ''}, 'id'
                cur['id'] = c_unescape(m.group(2)[1:-1])
            else:
                field = 'str'
                cur['str'] = c_unescape(m.group(2)[1:-1])
        elif line.startswith('"') and field:
            cur[field] += c_unescape(line[1:-1])
    flush()
    return entries


def write_po(path, keys, old, header):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(header)
        for k in keys:
            f.write('\nmsgid ' + po_quote(k) + '\nmsgstr ' + po_quote(old.get(k, '')) + '\n')
        unused = [k for k in old if k and k not in keys]
        if unused:
            f.write('\n# Plus utilisés (no longer used):\n')
            for k in unused:
                f.write('#~ msgid ' + po_quote(k).replace('\n', '\n#~ ') + '\n')


def main():
    keys = collect()
    lang = os.path.join(ROOT, 'lang')
    os.makedirs(lang, exist_ok=True)
    write_po(os.path.join(lang, 'messages.pot'), keys, {},
             '# Daytona Arcade Native: interface texts. The French text is the key.\n'
             '# Copy to <code>.po and fill each msgstr. Keep %d, %s, %% and \\n.\n')
    for path in sorted(glob.glob(os.path.join(lang, '*.po'))):
        code = os.path.basename(path)[:-3]
        old = parse_po(open(path, encoding='utf-8').read())
        write_po(path, keys, old, f'# Daytona Arcade Native: {code}\n')
    print(len(keys), 'texts')


if __name__ == '__main__':
    main()
