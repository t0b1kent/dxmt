#!/usr/bin/env python3
"""MacRunner c16 (17.10.2026): refuse literals of the shipped-1.0.9 WMTPixelFormat flag encoding in DXMT sources.

The AIR tree encodes swizzle/AlphaIsOne in bits 12..23 (CustomSwizzle 0xfff000); 1.0.9 used 0x800000 (AlphaIsOne),
0x400000/0x200000/0x100000 (R001/0R01/GBAR) and mask 0xf00000, e.g. BGRX8Unorm 0x800050. Code must use enum names.
Full composed values are refused anywhere; the single-bit values only on lines that talk about formats/swizzles
(they are also ordinary sizes such as 1 MiB = 0x100000). usage: check_format_abi.py <src dir>
"""
import re,sys
from pathlib import Path
COMPOSED=['0x800050','0x800051','0x400104','0x200105','0x10002a','0xf00000']
FLAGS=['0x800000','0x400000','0x200000','0x100000','8388608','4194304','2097152','1048576','15728640']
CONTEXT=re.compile(r'format|swizzle|alphaisone|bgrx|pixel',re.I)
def main(src):
    src=Path(src);bad=[]
    for p in sorted(src.rglob('*')):
        if p.suffix not in ('.c','.h','.cpp','.hpp','.m','.mm','.inc') or 'external' in p.parts:continue
        text=p.read_text(errors='replace')
        # Blank comments but keep line numbers, so documentation may name the old values.
        text=re.sub(r'/\*.*?\*/',lambda m:re.sub(r'[^\n]',' ',m.group(0)),text,flags=re.S)
        for n,line in enumerate(text.splitlines(),1):
            code=line.split('//')[0].lower()
            for v in COMPOSED+(FLAGS if CONTEXT.search(code) else []):
                pat=(r'\b0x0*'+v[2:] if v.startswith('0x') else r'\b'+v)+r'u?l*\b'
                if re.search(pat,code):bad.append(f'{p.relative_to(src)}:{n}: {v}')
    if bad:
        print('old (1.0.9) WMTPixelFormat literals:',*bad,sep='\n  ');return 1
    print('WMTPixelFormat encoding literals OK');return 0
if __name__=='__main__':sys.exit(main(sys.argv[1]))
