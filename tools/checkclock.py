import sys; sys.path.insert(0,'/home/kanan/vm/tools')
from ppmcheck import read_ppm
w,h,d=read_ppm(sys.argv[1])
src=open('/home/kanan/vm/asm/lib/fontlib.tal').read()
tbl=[]
for line in src.split('\n'):
    if line.startswith('        DB $'):
        for tok in line.split('DB ')[1].split(';')[0].split(','):
            tok=tok.strip()
            if tok.startswith('$'): tbl.append(int(tok[1:],16))
name="0123456789"
want=sys.argv[2] if len(sys.argv)>2 else None
def lit(x,y):
    k=(y*w+x)*3; return d[k:k+3]!=b'\x00\x00\x00'
ok=True
COLS = tuple(int(v) for v in sys.argv[3].split(',')) if len(sys.argv) > 3 \
         else (106, 122, 148, 164, 190, 206)
for k, x in enumerate(COLS):
    rows=[]
    for r in range(8):
        v=0
        for c in range(8):
            if lit(x+c,90+r): v |= 0x80>>c
        rows.append(v)
    hit=[i for i in range(40) if tbl[i*8:i*8+8]==rows]
    got = (name + '?')[hit[0]] if hit else '?'
    exp = want[k] if want else '?'
    flag = "ok" if (want is None or got==exp) else "WRONG"
    if flag!="ok": ok=False
    print("digit %d x=%3d -> '%s' want '%s'  %s"%(k,x,got,exp,flag))
print("ALL SIX CORRECT" if ok else "MISMATCH")
