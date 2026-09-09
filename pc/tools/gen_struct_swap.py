#!/usr/bin/env python3
"""
Generate byte-swap routines for the game's stage parameter blocks
("yakumono_param" in each stage archive) from the struct declarations in
src/melee/gr/*.c.

Each stage file declares its own parameter struct and reads it with
`X = Ground_GetYakumonoParam();`. This script parses that struct (following
nested struct types declared in the same file or in the gr/lb headers),
computes field offsets with natural alignment, checks them against the
`/* 0x.. */` offset comments where present, and writes:

  pc/generated/yakumono_swap.c    one pc_swap_yakumono_<file>() per stage
  pc/include/pc_yakumono_swap.h   their prototypes

It also inserts the call after the assignment in each stage file (inside a
TARGET_PC block) unless it is already there. Run from the repository root:

  python pc/tools/gen_struct_swap.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GR = os.path.join(ROOT, 'src', 'melee', 'gr')

# (size, alignment, kind) with kind: 4 = swap32, 2 = swap16, 0 = leave alone
BASIC = {
    'f32': (4, 4, 4), 'float': (4, 4, 4), 's32': (4, 4, 4), 'u32': (4, 4, 4),
    'int': (4, 4, 4), 'unsigned': (4, 4, 4), 'enum_t': (4, 4, 4), 'bool': (4, 4, 4),
    'size_t': (4, 4, 4), 'ssize_t': (4, 4, 4), 'GrKind': (4, 4, 4), 'StKind': (4, 4, 4),
    'UNK_T': (4, 4, 4),
    's16': (2, 2, 2), 'u16': (2, 2, 2),
    's8': (1, 1, 0), 'u8': (1, 1, 0), 'char': (1, 1, 0),
    'GXColor': (4, 1, 0),
}
COMPOSITE = {  # name: number of 32-bit words
    'Vec2': 2, 'Vec3': 3, 'Vec4': 4, 'Quaternion': 4, 'S32Vec3': 3,
}

member_re = re.compile(
    r'^\s*(?:/\*\s*\+?(?:0x)?([0-9A-Fa-f]+)\s*\*/\s*)?'  # optional offset comment
    r'(?:const\s+)?(struct\s+|union\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*(\*+)?\s*'
    r'([A-Za-z_][A-Za-z_0-9]*)((?:\s*\[[^\]]+\])*)\s*(?::\s*\d+)?\s*;')
anon_open_re = re.compile(r'^\s*(?:/\*\s*\+?(?:0x)?([0-9A-Fa-f]+)\s*\*/\s*)?struct\s*\{\s*$')
anon_close_re = re.compile(r'^\s*\}\s*([A-Za-z_][A-Za-z_0-9]*)((?:\s*\[[^\]]+\])*)\s*;')


def find_struct(name, texts):
    """Return the body lines of struct `name` from the given source texts."""
    pats = [r'^(?:/\*.*?\*/\s*)?(?:static\s+)?(?:typedef\s+)?struct\s+%s\s*\{\n(.*?)^\}' % re.escape(name),
            r'^typedef\s+struct\s*\{\n(.*?)^\}\s*%s\s*;' % re.escape(name),
            r'^typedef\s+struct\s+\w+\s*\{\n(.*?)^\}\s*%s\s*;' % re.escape(name)]
    for text in texts:
        for pat in pats:
            m = re.search(pat, text, re.S | re.M)
            if m:
                return m.group(1).split('\n')
    return None


def eval_dim(expr, text):
    expr = expr.strip()
    if re.match(r'^[0-9A-Fa-fx +\-*/()]+$', expr) and re.search(r'\d', expr):
        return int(eval(expr))  # noqa: S307 - digits and operators only
    m = re.search(r'#define\s+%s\s+\(?([0-9A-Fa-fx]+)\)?' % re.escape(expr), text)
    if m:
        return int(m.group(1), 0)
    m = re.search(r'\b%s\s*=\s*([0-9A-Fa-fx]+)' % re.escape(expr), text)
    if m:
        return int(m.group(1), 0)
    raise ValueError('array dimension ' + expr)


class Layout:
    def __init__(self):
        self.fields = []  # (offset, kind) with kind 4/2
        self.size = 0
        self.align = 1


def layout_of_body(body, name, texts, cache, path):
    lay = Layout()
    off = 0
    i = 0
    while i < len(body):
        raw = body[i]
        i += 1
        line = raw.split('//')[0]
        if not line.strip():
            continue
        am = anon_open_re.match(line)
        if am:
            sub_body = []
            depth = 1
            while i < len(body):
                l2 = body[i]
                i += 1
                if re.match(r'^\s*(struct|union)\s*\{', l2):
                    depth += 1
                cm = anon_close_re.match(l2)
                if cm and depth == 1:
                    break
                if re.match(r'^\s*\}', l2):
                    depth -= 1
                sub_body.append(l2)
            else:
                raise ValueError('%s: unterminated anonymous struct' % name)
            sub = layout_of_body(sub_body, name + '.' + cm.group(1), texts, cache, path)
            cmt_off, mname, dims = am.group(1), cm.group(1), cm.group(2)
            esize, ealign, ekinds = sub.size, sub.align, sub.fields
        else:
            m = member_re.match(line)
            if not m:
                if re.match(r'^\s*(/\*.*\*/)?\s*$', line):
                    continue
                raise ValueError('%s: cannot parse member: %s' % (name, line.strip()))
            cmt_off, tag, typ, stars, mname, dims = m.groups()
            if stars:
                esize, ealign, ekinds = 4, 4, [(0, 4)]
            elif typ in BASIC:
                esize, ealign, k = BASIC[typ]
                ekinds = [(0, k)] if k else []
            elif typ in COMPOSITE:
                n = COMPOSITE[typ]
                esize, ealign = 4 * n, 4
                ekinds = [(4 * j, 4) for j in range(n)]
            else:
                sub = layout_of(typ, texts, cache, path)
                esize, ealign, ekinds = sub.size, sub.align, sub.fields
        count = 1
        for d in re.findall(r'\[([^\]]+)\]', dims or ''):
            count *= eval_dim(d, texts[0])
        off = (off + ealign - 1) // ealign * ealign
        if cmt_off is not None and int(cmt_off, 16) != off:
            print('%s: %s.%s declared at 0x%X but computed 0x%X' % (path, name, mname, int(cmt_off, 16), off))
        for j in range(count):
            for sub_off, k in ekinds:
                lay.fields.append((off + j * esize + sub_off, k))
        off += esize * count
        lay.align = max(lay.align, ealign)
    lay.size = (off + lay.align - 1) // lay.align * lay.align
    return lay


def layout_of(name, texts, cache, path):
    if name in cache:
        return cache[name]
    body = find_struct(name, texts)
    if body is None:
        raise ValueError('struct %s not found' % name)
    lay = layout_of_body(body, name, texts, cache, path)
    cache[name] = lay
    return lay


def emit(fname, lay):
    out = ['void %s(void* p)' % fname, '{', '    u8* b = (u8*) p;',
           '    if (p == NULL || !pc_swap_once(p)) {', '        return;', '    }']
    fields = sorted(set(lay.fields))
    i = 0
    while i < len(fields):
        off, k = fields[i]
        j = i
        while j + 1 < len(fields) and fields[j + 1][1] == k and fields[j + 1][0] == fields[j][0] + k:
            j += 1
        n = j - i + 1
        if k == 4:
            # relocated pointer slots are skipped at run time: several
            # structs declare pointers as plain ints
            out.append('    swap_words(b + 0x%X, %d);' % (off, n))
        elif n == 1:
            out.append('    pc_swap16(b + 0x%X);' % off)
        else:
            out.append('    pc_swap16_range(b + 0x%X, 0x%X);' % (off, n * 2))
        i = j + 1
    out.append('}')
    return '\n'.join(out)


# variable -> struct type for the holders that are not the usual static pointer
SPECIAL = {
    'grgreatbay.c': ('grGb_804D69E0.x0', 'grGb_StageAttr'),
    'grheal.c': ('grHeal_804D6AF0[0]', 'grHeal_UnkData'),
    'gryorster.c': ('grYt_804D6A20.x0', 'YorsterParams'),
}


def variable_type(var, text, headers):
    pats = [r'(?:/\*[^*]*\*/\s*)?(?:static\s+)?(?:struct\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*\*\s*%s\s*(?:=\s*NULL)?\s*;' % re.escape(var),
            r'(?:static\s+)?struct\s+([A-Za-z_][A-Za-z_0-9]*)\s*\{[^}]*\}\s*\*\s*%s\s*;' % re.escape(var)]
    for t in [text] + headers:
        for pat in pats:
            m = re.search(pat, t, re.S)
            if m:
                return m.group(1)
    return None


def main():
    headers = []
    for d in [GR, os.path.join(ROOT, 'src', 'melee', 'lb'), os.path.join(ROOT, 'src', 'melee', 'it')]:
        for f in sorted(os.listdir(d)):
            if f.endswith('.h'):
                headers.append(open(os.path.join(d, f), encoding='utf-8', errors='replace').read())
    funcs, protos = [], []
    for f in sorted(os.listdir(GR)):
        if not f.endswith('.c'):
            continue
        path = os.path.join(GR, f)
        text = open(path, encoding='utf-8').read()
        m = re.search(r'^([ \t]*)([A-Za-z_][A-Za-z_0-9.\[\]]*) = Ground_GetYakumonoParam\(\);[ \t]*$', text, re.M)
        if not m:
            continue
        indent, var = m.group(1), m.group(2)
        if f in SPECIAL:
            var, typ = SPECIAL[f]
        else:
            typ = variable_type(var, text, headers)
            if typ is None:
                print('%s: cannot find the type of %s' % (f, var))
                continue
        if typ in ('void', 'int'):
            print('%s: %s is a %s pointer -- skipped' % (f, var, typ))
            continue
        try:
            lay = layout_of(typ, [text] + headers, {}, f)
        except ValueError as e:
            print('%s: %s (%s) -- skipped' % (f, e, typ))
            continue
        fname = 'pc_swap_yakumono_' + f[:-2]
        funcs.append('/* %s: %s, 0x%X bytes */\n' % (f, typ, lay.size) + emit(fname, lay))
        protos.append('void %s(void* p);' % fname)
        call = '#ifdef TARGET_PC\n%s%s(%s);\n#endif\n' % (indent, fname, var)
        if fname + '(' not in text:
            text = text[:m.end()] + '\n' + call.rstrip('\n') + text[m.end():]
            if 'pc_yakumono_swap.h' not in text:
                k = text.index('#include')
                text = text[:k] + '#ifdef TARGET_PC\n#include <pc_yakumono_swap.h>\n#endif\n' + text[k:]
            open(path, 'w', encoding='utf-8', newline='\n').write(text)
    gen_c = os.path.join(ROOT, 'pc', 'generated', 'yakumono_swap.c')
    gen_h = os.path.join(ROOT, 'pc', 'include', 'pc_yakumono_swap.h')
    with open(gen_c, 'w', newline='\n') as o:
        o.write('/* Generated by pc/tools/gen_struct_swap.py: byte-swappers for the\n'
                ' * stage parameter blocks, derived from the structs in src/melee/gr. */\n'
                '#include <pc_endian.h>\n#include <pc_hsd_swap.h>\n#include <pc_yakumono_swap.h>\n\n'
                '#include <stddef.h>\n\ntypedef unsigned char u8;\n\n'
                '/* 32-bit fields; pointer slots relocated by the archive loader are skipped */\n'
                'static void swap_words(u8* p, int n)\n{\n    int i;\n    for (i = 0; i < n; i++) {\n'
                '        if (!pc_swap_is_reloc_slot(p + i * 4)) {\n            pc_swap32(p + i * 4);\n'
                '        }\n    }\n}\n\n')
        o.write('\n\n'.join(funcs) + '\n')
    with open(gen_h, 'w', newline='\n') as o:
        o.write('/* Generated by pc/tools/gen_struct_swap.py. */\n#ifndef PC_YAKUMONO_SWAP_H\n'
                '#define PC_YAKUMONO_SWAP_H\n\n' + '\n'.join(protos) + '\n\n#endif\n')
    print('generated %d swappers' % len(funcs))


if __name__ == '__main__':
    main()
