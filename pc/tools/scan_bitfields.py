"""Find structs where u32/s32/int bit-fields are followed by smaller plain
members. Metrowerks packs the plain member into the bit-fields' 32-bit unit,
MSVC starts a new unit, so the struct sizes and offsets differ on PC. Fix a
hit by declaring the bit-fields byte-sized under TARGET_PC (see
StartMeleeRules in src/melee/mn/types.h).

    python pc/tools/scan_bitfields.py
"""
import pathlib, re, sys
root = pathlib.Path(__file__).resolve().parents[2] / 'src'
struct_re = re.compile(r'(struct\s+\w*\s*\{|typedef\s+struct\s+\w*\s*\{)(.*?)\n\}', re.S)
bf_re = re.compile(r'^\s*(u32|s32|int|unsigned int|unsigned)\s+\w+\s*:\s*(\d+)\s*;')
plain_re = re.compile(r'^\s*(u8|s8|u16|s16|char|short|unsigned char|unsigned short|bool)\s+\w+\s*(\[[^\]]*\])?\s*;')
hits = 0
for path in sorted(root.rglob('*.[ch]')):
    text = path.read_text(errors='replace')
    for m in struct_re.finditer(text):
        body = m.group(2)
        lines = body.split('\n')
        bits = 0
        in_bf = False
        for i, line in enumerate(lines):
            line = line.split('//')[0]
            b = bf_re.match(line)
            if b:
                bits += int(b.group(2))
                in_bf = True
                continue
            p = plain_re.match(line)
            if in_bf and p and bits % 32 != 0:
                rel = text[:m.start()].count('\n') + 1
                print('%s:%d: %s bits then %s' % (path.relative_to(root), rel + i + 1, bits, p.group(0).strip()))
                hits += 1
                in_bf = False
                break
            st = line.strip()
            if st and not st.startswith('/*') and not st.startswith('*') and not st.startswith('//') and not st.startswith('#'):
                in_bf = False
                bits = 0
print(hits, 'candidates')
