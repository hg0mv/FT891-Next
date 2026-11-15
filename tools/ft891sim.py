#!/usr/bin/env python3
"""FT-891 szimulátor pty-n, a Yaesu CAT-könyv (2017) alapján. Valódi rádiót NEM helyettesít.

Használat:  python3 tools/ft891sim.py [--link /tmp/ft891] [--baud 38400] [hibák]
A vezérlőt ezzel indítsd:  ./ft891ctl --cat-port /tmp/ft891

Hibák:
  --old-menu       korábbi menütábla (nincs 11-09, a 11-07 az SSB PTT SELECT)
  --ptt-drop N     CAT PTT után N kör múlva kiesik az adásból
  --mute-file ÚT   ha ez a fájl létezik, a rádió néma (kikapcsolt rádió)
"""
import argparse, os, pty, re, select, sys, termios, time, tty

def menu_defaults():
    """A rádió alapértékei a menus.def alapján"""
    out = {}
    here = os.path.dirname(os.path.abspath(__file__))
    for l in open(os.path.join(here, '..', 'src', 'menus.def'), encoding='utf-8'):
        m = re.match(r'M\("(\d+)",".*?",".*?",".*?",(K_\w+),(\d+),(-?\d+),(-?\d+)', l)
        if not m: continue
        i, k, d, lo = m[1], m[2], int(m[3]), int(m[4])
        if i == '1701': continue
        if k == 'K_SIGN': out[i] = '+' + '0' * (d - 1)
        elif k == 'K_OFFSTEP': out[i] = '0' * (d - 1) + '1'
        else: out[i] = f'{lo:0{d}d}'
    return out

class Rig:
    def __init__(s, a):
        s.a = a
        s.fa, s.fb = 14074000, 7074000
        s.md = '2'; s.ai = 0; s.tx = 0; s.split = 0; s.clar = 0; s.ofs = 0
        s.lv = dict(AG=120, RG=30, SQ=0, PC=50, MG=50, PL=50, RL=8, NL=5)
        s.fl = dict(NR=0, NB=0, BC=0, PA=0, RA=0, PR=0, LK=0)
        s.gt = 4; s.na = 0; s.sh = 14
        s.on = not a.off; s.last_rx = 0
        s.kp = 40; s.isv = '0+0000'; s.bp = {'0': '000', '1': '150'}
        s.co = {'0': '0000', '1': '1000', '2': '0000', '3': '0025'}
        s.ex = {'0506':'3','0507':'2','0508':'0','0707':'2','0801':'1','0809':'1','0810':'0',
                '0812':'0','1011':'0','1105':'0','1108':'0','1801':'0115','1802':'0112','1803':'0108'}
        s.ex = {**menu_defaults(), **s.ex}
        s.mem = {'001': dict(f=14074000, md='C', tag='FT8 20M', shift=0), '002': dict(f=7074000, md='8', tag='', shift=0)}
        s.ch = '001'
        if a.old_menu:
            del s.ex['1109']
            s.ex['1107'] = '0'          # SSB PTT SELECT: DAKY
            s.ex['1108'] = '3'
        else:
            s.ex['1107'] = '2'; s.ex['1109'] = '3'
        s.drop_at = None; s.log = open(a.cmdlog, 'a', buffering=1) if a.cmdlog else None
        s.rx_count = 0

    def info(s):
        md = s.md
        return f"IF001{s.fa:09d}{'+' if s.ofs >= 0 else '-'}{abs(s.ofs):04d}{s.clar}0{md}000000;"

    def handle(s, c):
        if s.log: s.log.write(c + '\n')
        now = time.time(); gap = now - s.last_rx; s.last_rx = now
        if not s.on:
            # a könyv szerint: tetszőleges adat, majd 1-2 mp múlva a parancs
            if c == 'PS1;' and 1 <= gap <= 2: s.on = True
            return ''
        if c == 'PS0;': s.on = False; s.tx = 0; return ''
        if c == 'PS;': return 'PS1;'
        s.rx_count += 1
        k, p = c[:2], c[2:-1]
        out, ai = '', ''
        def lvl(nm, wid, zero):
            nonlocal out, ai
            pre = '0' if zero else ''
            body = p[1:] if zero else p
            if body:
                s.lv[nm] = int(body); ai = f"{nm}{pre}{int(body):0{wid}d};"
            else:
                out = f"{nm}{pre}{s.lv[nm]:0{wid}d};"
        if k == 'ID': out = 'ID0650;'
        elif k == 'AI':
            if p: s.ai = int(p)
            else: out = f'AI{s.ai};'
        elif k == 'FA':
            if p: s.fa = int(p); ai = s.info()
            else: out = f'FA{s.fa:09d};'
        elif k == 'FB':
            if p: s.fb = int(p)
            else: out = f'FB{s.fb:09d};'
        elif k == 'IF': out = s.info()
        elif k == 'MD':
            if len(p) == 2: s.md = p[1]; ai = f'MD0{s.md};'
            else: out = f'MD0{s.md};'
        elif k == 'SM': out = f'SM0{(int(time.time()*4) % 40) * 4 + 40:03d};'
        elif k == 'RM':
            out = f'RM{p}{ {"5":120,"6":30,"4":40}.get(p, 0) if s.tx else 0:03d};'
        elif k == 'TX':
            if p:
                s.tx = int(p)
                if s.tx and s.a.ptt_drop: s.drop_at = s.rx_count + s.a.ptt_drop
            else:
                if s.drop_at and s.rx_count >= s.drop_at: s.tx = 0; s.drop_at = None
                out = f'TX{s.tx};'
        elif k in ('AG', 'RG', 'SQ', 'NL'): lvl(k, 3, True)
        elif k == 'RL': lvl(k, 2, True)
        elif k in ('PC', 'MG', 'PL'): lvl(k, 3, False)
        elif k in ('NR', 'NB', 'BC', 'PA', 'RA'):
            if len(p) == 2: s.fl[k] = int(p[1]); ai = f'{k}0{p[1]};'
            else: out = f'{k}0{s.fl[k]};'
        elif k == 'PR':
            kk = 'PR' + p[0]
            if len(p) == 2: s.fl[kk] = int(p[1])
            else: out = f'PR{p[0]}{s.fl.get(kk, 0)};'
        elif k == 'KP':
            if p: s.kp = int(p)
            else: out = f'KP{s.kp:02d};'
        elif k == 'IS':
            if len(p) > 1: s.isv = p[1:]
            else: out = f'IS0{s.isv};'
        elif k == 'BP':
            if len(p) > 2: s.bp[p[1]] = p[2:]
            else: out = f'BP0{p[1]}{s.bp[p[1]]};'
        elif k == 'CO':
            if len(p) > 2: s.co[p[1]] = p[2:]
            else: out = f'CO0{p[1]}{s.co[p[1]]};'
        elif k == 'LK':
            if p: s.fl[k] = int(p)
            else: out = f"LK{s.fl[k]};"
        elif k == 'GT':
            if len(p) == 2: s.gt = int(p[1])
            else: out = f'GT0{s.gt};'
        elif k == 'NA':
            if len(p) == 2: s.na = int(p[1])
            else: out = f'NA0{s.na};'
        elif k == 'SH':
            if len(p) == 4: s.sh = int(p[2:])
            else: out = f'SH01{s.sh:02d};'
        elif k == 'ST':
            if p: s.split = int(p)
            else: out = f'ST{s.split};'
        elif k == 'CF':
            if len(p) == 3: s.clar = int(p[1])
            else: out = f'CF0{s.clar}0;'
        elif k == 'RC': s.ofs = 0
        elif k == 'RU': s.ofs += int(p)
        elif k == 'RD': s.ofs -= int(p)
        elif k == 'SV': s.fa, s.fb = s.fb, s.fa
        elif k == 'AB': s.fb = s.fa
        elif k == 'BA': s.fa = s.fb
        elif k == 'BS':
            s.fa = {0:1840000,1:3573000,3:7074000,4:10136000,5:14074000,6:18100000,7:21074000,
                    8:24915000,9:28074000,10:50313000}.get(int(p), s.fa)
        elif k == 'MC':
            if p: s.ch = p
            else: out = f'MC{s.ch};'
        elif k == 'MA':
            m = s.mem.get(s.ch)
            if m: s.fa, s.md = m['f'], m['md']
        elif k in ('MT', 'MR'):
            ch = p[:3]
            if len(p) > 3:
                m = re.fullmatch(r'(.{3})(\d{9})([+-])(\d{4})(\d)0(.)0(\d)00(\d)(\d)(.{12})', p)
                if not m: return '?;'
                s.mem[ch] = dict(f=int(m[2]), md=m[6], tag=m[10].rstrip() if m[9] == '1' else '', shift=int(m[8]))
            elif ch not in s.mem: out = '?;'
            else:
                m = s.mem[ch]
                b = f"{k}{ch}{m['f']:09d}+0000" + f"00{m['md']}0000{m['shift']}"
                out = b + (f"{1 if m['tag'] else 0}{m['tag']:<12}" if k == 'MT' else '') + ';'
        elif k == 'EX':
            m, v = p[:4], p[4:]
            if m not in s.ex: out = '?;'
            elif v: s.ex[m] = v
            else: out = f'EX{m}{s.ex[m]};'
        else: out = '?;'
        return out + (ai if s.ai else '')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--link', default='/tmp/ft891')
    ap.add_argument('--baud', type=int, default=38400)
    ap.add_argument('--old-menu', action='store_true')
    ap.add_argument('--ptt-drop', type=int, default=0)
    ap.add_argument('--mute-file')
    ap.add_argument('--off', action='store_true', help='kikapcsolt rádió')
    ap.add_argument('--cmdlog')
    ap.add_argument('--state-file')
    a = ap.parse_args()
    m, sl = pty.openpty()
    tty.setraw(sl)
    name = os.ttyname(sl)
    if os.path.islink(a.link) or os.path.exists(a.link): os.unlink(a.link)
    os.symlink(name, a.link)
    print('FT-891 szimulátor:', a.link, '->', name, flush=True)
    speeds = {4800: termios.B4800, 9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400}
    rig = Rig(a); buf = ''
    while True:
        r, _, _ = select.select([m], [], [], 0.2)
        if a.state_file:
            with open(a.state_file + '.new', 'w') as f:
                f.write(repr(dict(fa=rig.fa, fb=rig.fb, md=rig.md, tx=rig.tx, ex=rig.ex, ai=rig.ai,
                                  sh=rig.sh, na=rig.na, split=rig.split, lv=rig.lv, fl=rig.fl, power=rig.on, mem=rig.mem, ch=rig.ch, isv=rig.isv, bp=rig.bp, co=rig.co)))
            os.replace(a.state_file + '.new', a.state_file)
        if not r: continue
        try: d = os.read(m, 256)
        except OSError: time.sleep(0.05); continue
        if a.mute_file and os.path.exists(a.mute_file): buf = ''; continue
        # a pty megjegyzi a beállított sebességet: rossz baudnál nincs értelmes válasz
        if termios.tcgetattr(m)[4] != speeds[a.baud]: continue
        buf += d.decode('ascii', 'replace')
        while ';' in buf:
            c, buf = buf.split(';', 1)
            out = rig.handle(c + ';')
            if out:
                time.sleep(0.003)
                os.write(m, out.encode())

if __name__ == '__main__':
    main()
