#!/usr/bin/env python3
"""Teljes körű teszt a szimulátorral. Valódi rádiót nem helyettesít. Futtatás: make test"""
import ast, base64, math, ssl, zlib, http.client, json, os, shutil, signal, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get('FT891CTL', os.path.join(ROOT, 'ft891ctl'))
WEB, RIG = 18891, 14532
results = []

class Stack:
    def __init__(s, *simargs, ctl=(), nowait=False):
        s.d = tempfile.mkdtemp(prefix='ft891t')
        s.link = s.d + '/rig'; s.stf = s.d + '/state'; s.cmd = s.d + '/cmds'; s.mute = s.d + '/mute'
        s.sim = subprocess.Popen([sys.executable, ROOT + '/tools/ft891sim.py', '--link', s.link,
                                  '--state-file', s.stf, '--cmdlog', s.cmd, '--mute-file', s.mute, *simargs],
                                 stdout=subprocess.DEVNULL)
        for _ in range(50):
            if os.path.exists(s.link): break
            time.sleep(.05)
        s.log = open(s.d + '/ctl.log', 'w')
        s.ctl = subprocess.Popen([BIN, '--cat-port', s.link, '--web-port', str(WEB), '--rig-port', str(RIG),
                                  '--config-dir', s.d + '/cfg', *ctl], stdout=s.log, stderr=s.log)
        if nowait: time.sleep(1); return
        wait(lambda: state(s)['connected'] and state(s)['menu_n_done'], 8, 'kapcsolódás')
    def rig(s): return ast.literal_eval(open(s.stf).read())
    def cmds(s): return open(s.cmd).read().split('\n')
    def pin(s): return [l for l in open(s.d + '/cfg/config') if l.startswith('pin=')][0][4:].strip()
    def close(s):
        if s.ctl.poll() is None: s.ctl.terminate(); s.ctl.wait(5)
        s.sim.kill(); s.sim.wait(); s.log.close()
        rc = s.ctl.returncode
        shutil.rmtree(s.d, ignore_errors=True)
        return rc
    def __enter__(s): return s
    def __exit__(s, *a): s.close()

def http_req(method, path, body=None, headers=None, host='127.0.0.1'):
    c = http.client.HTTPConnection(host, WEB, timeout=3)
    c.request(method, path, body, headers or {})
    r = c.getresponse(); d = r.read(); c.close()
    return r.status, d, dict(r.getheaders())

def state(_s=None):
    try:
        st, d, _ = http_req('GET', '/api/state')
    except OSError:
        return dict(connected=0, menu_n_done=0)
    j = json.loads(d)
    j['menu_n_done'] = len(j['menu']) >= 11 or not j['menu_ok']
    return j

def wait(fn, t, what):
    end = time.time() + t
    while time.time() < end:
        try:
            if fn(): return
        except (OSError, ValueError, SyntaxError): pass
        time.sleep(.05)
    raise AssertionError('időtúllépés: ' + what)

class WS:
    def __init__(s, host='127.0.0.1', headers='', tls=False):
        s.k = socket.create_connection((host, WEB + 1 if tls else WEB), timeout=3)
        if tls:
            ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
            s.k = ctx.wrap_socket(s.k)
        key = base64.b64encode(os.urandom(16)).decode()
        s.k.sendall((f'GET /ws HTTP/1.1\r\nHost: {host}:{WEB}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                     f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{headers}\r\n').encode())
        s.head = b''
        while b'\r\n\r\n' not in s.head:
            d = s.k.recv(2048)
            if not d: break
            s.head += d
        s.code = int(s.head.split()[1]) if s.head else 0
    def send(s, t):
        d = t.encode(); m = os.urandom(4)
        s.k.sendall(bytes([0x81, 0x80 | len(d)]) + m + bytes(c ^ m[i % 4] for i, c in enumerate(d)))
    def close(s): s.k.close()
    def send_bin(s, d):
        m = os.urandom(4); h = bytes([0x82, 0x80 | 126]) + len(d).to_bytes(2, 'big') if len(d) > 125 else bytes([0x82, 0x80 | len(d)])
        s.k.sendall(h + m + bytes(c ^ m[i % 4] for i, c in enumerate(d)))
    def frames(s, secs):
        """Beérkező keretek: (típus, adat)"""
        end = time.time() + secs; s.k.settimeout(.2); buf = b''; out = []
        while time.time() < end:
            try: buf += s.k.recv(65536)
            except socket.timeout: continue
            while len(buf) >= 2:
                op, n, h = buf[0] & 15, buf[1] & 127, 2
                if n == 126:
                    if len(buf) < 4: break
                    n = int.from_bytes(buf[2:4], 'big'); h = 4
                if len(buf) < h + n: break
                out.append((op, buf[h:h + n])); buf = buf[h + n:]
        return out

def rigctl(lines, keep=False):
    k = socket.create_connection(('127.0.0.1', RIG), timeout=3); out = []
    for l in lines:
        k.sendall((l + '\n').encode()); time.sleep(.2)
        k.settimeout(.6)
        try: out.append(k.recv(8192).decode())
        except socket.timeout: out.append('')
    if keep: return k, out
    k.close()
    return out

def running(name):
    """Fut-e olyan folyamat, amelynek a programneve ez (a hamis program Python alatt fut)"""
    for p in os.listdir('/proc'):
        if not p.isdigit(): continue
        try: a = open(f'/proc/{p}/cmdline', 'rb').read().split(b'\0')
        except OSError: continue
        if len(a) > 2 and a[1].endswith(b'/' + name.encode()) and a[2] == b'-q': return True
    return False

def test(fn):
    only = os.environ.get('TEST_ONLY')
    if only and only not in fn.__name__: return fn
    t = time.time()
    try:
        fn(); results.append((fn.__name__, True, ''))
        print(f'  rendben  {fn.__doc__}  ({time.time()-t:.1f} mp)', flush=True)
    except Exception as e:
        results.append((fn.__name__, False, repr(e)))
        import traceback
        ln = [f.lineno for f in traceback.extract_tb(e.__traceback__) if f.filename == __file__][-1]
        print(f'  HIBA     {fn.__doc__}: {e!r} (sor: {ln})', flush=True)
    return fn

@test
def t_autobaud():
    """A sebességet maga találja meg (9600 baud)"""
    with Stack('--baud', '9600'):
        assert state()['baud'] == 9600

@test
def t_sideband():
    """USB/LSB/Automatikus a BFO-menün át, az AUTO megőrzésével"""
    with Stack() as s:
        w = WS(); assert s.rig()['ex']['1107'] == '2'
        w.send('mode 1'); time.sleep(.5)            # 14 MHz, AUTO: már USB
        assert s.rig()['ex']['1107'] == '2', 'az AUTO elveszett'
        assert 'EX11070;' not in s.cmds()
        w.send('mode 0'); wait(lambda: state()['mode'] == 'LSB', 2, 'LSB')
        assert s.rig()['ex']['1107'] == '1'
        w.send('mode 1'); wait(lambda: state()['mode'] == 'USB', 2, 'USB')
        assert s.rig()['ex']['1107'] == '0'
        w.send('mode 2'); wait(lambda: s.rig()['ex']['1107'] == '2', 2, 'auto')
        w.send('band 3'); wait(lambda: state()['fa'] == 7074000, 2, '40 m')
        wait(lambda: state()['mode'] == 'LSB', 2, 'auto LSB 40 m-en')
        rigctl(['M LSB 0']); assert s.rig()['ex']['1107'] == '2'
        rigctl(['M PKTUSB 0']); wait(lambda: state()['mode'] == 'DATA-U', 2, 'DATA-U')
        assert rigctl(['m'])[0].split()[0] == 'PKTUSB'
        w.close()

@test
def t_old_menu():
    """Korábbi menütáblánál a 11-07-et sosem írja felül"""
    with Stack('--old-menu') as s:
        wait(lambda: 'eltérő' in state()['notice'].lower(), 3, 'figyelmeztetés'); assert state()['menu_ok'] == 0
        w = WS(); w.send('mode 0'); time.sleep(.5); w.send('mode 1'); time.sleep(.5); w.send('mode 2'); time.sleep(.5)
        assert not [c for c in s.cmds() if c.startswith('EX1107') and len(c) > 7], 'írt a 11-07-re'
        assert s.rig()['ex']['1107'] == '0' and 'nem állítható' in state()['notice']
        w.close()

@test
def t_web_ptt_lease():
    """A webes PTT elenged, ha elmarad a jel vagy megszakad a kapcsolat"""
    with Stack() as s:
        w = WS(); w.send('ptt 1'); wait(lambda: s.rig()['tx'] == 1, 1, 'adás be')
        t0 = time.time(); wait(lambda: s.rig()['tx'] == 0, 2.5, 'lease lejárt')
        assert .8 < time.time() - t0 < 2.2
        w.send('ptt 1'); wait(lambda: s.rig()['tx'] == 1, 1, 'adás be 2')
        for _ in range(8): w.send('ptt 1'); time.sleep(.4); assert s.rig()['tx'] == 1, 'tartás közben leesett'
        w.close(); wait(lambda: s.rig()['tx'] == 0, 1.5, 'ws zárás')

@test
def t_rigctl_ptt_disconnect():
    """A rigctld-ügyfél kilépésekor nem marad adásban"""
    with Stack() as s:
        k, _ = rigctl(['T 1'], keep=True); wait(lambda: s.rig()['tx'] == 1, 1, 'adás be')
        k.close(); wait(lambda: s.rig()['tx'] == 0, 1.5, 'ügyfél kilépett')

@test
def t_tx_guard():
    """Az adásidő-korlát kikapcsol, és felengedésig nem indul újra"""
    with Stack() as s:
        w = WS(); w.send('txmax 10'); wait(lambda: state()['tx_max'] == 10, 1, 'korlát')
        k, _ = rigctl(['T 1'], keep=True); wait(lambda: s.rig()['tx'] == 1, 1, 'adás be')
        wait(lambda: s.rig()['tx'] == 0, 11.5, 'korlát lejárt')
        time.sleep(1); assert s.rig()['tx'] == 0, 'újra adásba ment'
        k.sendall(b'T 0\n'); time.sleep(.3); k.sendall(b'T 1\n'); wait(lambda: s.rig()['tx'] == 1, 1, 'új adás')
        k.close(); w.close()

@test
def t_ptt_drop():
    """A leeső CAT PTT-t észreveszi, kétszer újrapróbálja, aztán szól"""
    with Stack('--ptt-drop', '3') as s:
        k, _ = rigctl(['T 1'], keep=True)
        wait(lambda: 'RTS' in state()['notice'], 6, 'figyelmeztetés')
        j = state(); assert j['drops'] == 3, j['drops']
        assert s.cmds().count('TX1;') == 3
        wait(lambda: s.rig()['tx'] == 0, 1, 'nincs adás'); k.close()

@test
def t_ptt_test():
    """A PTT-teszt 1 mp után kikapcsol és eredményt mond"""
    with Stack() as s:
        w = WS(); w.send('ptttest'); wait(lambda: s.rig()['tx'] == 1, 1, 'teszt adás')
        wait(lambda: 'sikerült' in state()['notice'], 3, 'eredmény')
        assert s.rig()['tx'] == 0; w.close()

@test
def t_mode_lock():
    """A módzár a külső programot letiltja, a felületet nem"""
    with Stack() as s:
        w = WS(); w.send('modelock 1'); wait(lambda: state()['mode_lock'] == 1, 1, 'módzár')
        assert 'RPRT 0' in rigctl(['M CW 0'])[0]; time.sleep(.3); assert state()['mode'] == 'USB'
        w.send('mode 3'); wait(lambda: state()['mode'].startswith('CW'), 2, 'kézi váltás'); w.close()

@test
def t_reconnect():
    """Kikapcsolt rádiónál elereszti az adást, visszatéréskor nem ad újra"""
    with Stack() as s:
        k, _ = rigctl(['T 1'], keep=True); wait(lambda: s.rig()['tx'] == 1, 1, 'adás be')
        open(s.mute, 'w').close()
        wait(lambda: not state()['connected'], 4, 'kiesés')
        assert 'RPRT -6' in rigctl(['f'])[0]
        os.unlink(s.mute); s.sim.send_signal(signal.SIGCONT)
        wait(lambda: state()['connected'], 8, 'visszatérés')
        time.sleep(1); assert state()['keyed'] == 0; k.close()

@test
def t_controls():
    """Szintek, kapcsolók, szűrő, VFO, split, clarifier eljut a rádióig"""
    with Stack() as s:
        w = WS()
        for c in ['lvl 0 200', 'lvl 3 40', 'fn 0 1', 'fn 3 1', 'agc 2', 'width 21', 'split 1', 'clar 1', 'clarstep 50',
                  'a2b']: w.send(c)
        def ok():
            r = s.rig(); j = state()
            return (r['lv']['AG'] == 200 and r['lv']['PC'] == 40 and r['fl']['NR'] == 1 and r['fl']['PA'] == 1 and
                    r['sh'] == 21 and r['na'] == 0 and r['split'] == 1 and r['fb'] == r['fa'] and j['agc'] == 2 and
                    j['clar'] == 1 and j['clar_ofs'] == 50 and j['width_hz'] == 3200)
        wait(ok, 6, 'minden eljutott'); w.close()

@test
def t_coalesce():
    """A gyors csúszkamozgás nem árasztja el a rádiót"""
    with Stack() as s:
        w = WS(); n0 = len(s.cmds())
        for i in range(60): w.send(f'lvl 0 {i}')
        wait(lambda: s.rig()['lv']['AG'] == 59, 3, 'utolsó érték')
        ag = [c for c in s.cmds()[n0:] if c.startswith('AG0') and len(c) > 4]
        assert len(ag) < 15, len(ag); w.close()

@test
def t_polling_rate():
    """Lekérdezési terhelés ügyfél nélkül és nyitott felülettel"""
    with Stack() as s:
        time.sleep(3); a = len(s.cmds()); time.sleep(5); idle = (len(s.cmds()) - a) / 5
        w = WS(); time.sleep(1); a = len(s.cmds()); time.sleep(5); act = (len(s.cmds()) - a) / 5; w.close()
        print(f'           üresen {idle:.1f} parancs/mp, felülettel {act:.1f} parancs/mp')
        assert idle < 5 and act < 16

@test
def t_rigctld_protocol():
    """rigctld-parancsok és a dump_state szerkezete"""
    with Stack():
        o = rigctl(['\\chk_vfo', 'v', 's', 'F 14074000', 'f', '+f', 'I 14075000', 'i', 'S 1 VFOB', 's', 'S 0 VFOA',
                    '\\get_powerstat', 'l STRENGTH', 'L RFPOWER 0.30', 'l RFPOWER', 'w FA;', 'q'])
        assert o[0] == '0\n' and o[1] == 'VFOA\n' and o[2] == '0\nVFOA\n' and o[3] == 'RPRT 0\n'
        assert o[4] == '14074000\n' and o[5] == 'get_freq:\nFrequency: 14074000\nRPRT 0\n'
        assert o[7] == '14075000\n' and o[9] == '1\nVFOB\n' and o[11] == '1\n'
        assert o[13] == 'RPRT 0\n' and o[14] == '0.30\n' and o[15].startswith('RPRT -11')
        d = rigctl(['\\dump_state'])[0].split('\n')
        assert d[0] == '1' and d[1] == '1036' and d[2] == '0' and d[3].split()[1] == '56000000' and d[-2] == 'done'
        assert d.index('0 0 0 0 0 0 0') == 4

@test
def t_auth():
    """PIN, idegen eredet, DNS-rebinding, kitiltás"""
    with Stack() as s:
        lan = [a for a in socket.gethostbyname_ex(socket.gethostname())[2] if not a.startswith('127.')]
        lan = lan[0] if lan else subprocess.check_output(['hostname', '-I']).decode().split()[0]
        assert http_req('GET', '/api/state', host=lan)[0] == 401
        assert WS(host=lan).code == 401
        assert http_req('GET', '/api/state', headers={'Host': 'evil.example:18891'})[0] == 401, 'rebinding'
        assert WS(headers='Origin: http://evil.example\r\n').code == 403
        st, _, h = http_req('POST', '/api/login', 'pin=' + s.pin(), host=lan)
        assert st == 200 and 'HttpOnly' in h['Set-Cookie'] and 'SameSite=Strict' in h['Set-Cookie']
        ck = h['Set-Cookie'].split(';')[0]
        assert http_req('GET', '/api/state', headers={'Cookie': ck}, host=lan)[0] == 200
        w = WS(host=lan, headers=f'Cookie: {ck}\r\nOrigin: http://{lan}:{WEB}\r\n'); assert w.code == 101; w.close()
        assert http_req('GET', '/api/state', headers={'Cookie': 'ft891=' + 'a' * 32}, host=lan)[0] == 401
        for _ in range(5): assert http_req('POST', '/api/login', 'pin=000', host=lan)[0] == 403
        assert http_req('POST', '/api/login', 'pin=' + s.pin(), host=lan)[0] == 429
        assert s.pin() not in http_req('GET', '/')[1].decode()

@test
def t_cross_protocol():
    """HTTP-kérés nem kapcsolhat adásba a rigctld-porton át"""
    with Stack() as s:
        k = socket.create_connection(('127.0.0.1', RIG), timeout=2)
        k.sendall(b'POST / HTTP/1.1\r\nHost: 127.0.0.1:14532\r\nContent-Type: text/plain\r\n\r\nT 1\n')
        time.sleep(.5); assert s.rig()['tx'] == 0
        k.close()

@test
def t_log_utf8():
    """A hosszú, ékezetes napló sem bontja el a webes kapcsolatot"""
    with Stack():
        w = WS()
        for _ in range(40): http_req('POST', '/api/login', 'pin=000')
        time.sleep(.5); assert http_req('GET', '/api/state')[0] == 200
        w.k.settimeout(1); d = b''
        try:
            while len(d) < 200000: d += w.k.recv(65536)
        except socket.timeout: pass
        assert 'Hibás PIN'.encode() in d, 'a friss napló nem jött meg'
        w.close()

def api(path, body=None):
    st, d, _ = http_req('POST' if body is not None else 'GET', path, body)
    return st, json.loads(d)

def sweep(s):
    w = WS(); w.send('menusweep')
    wait(lambda: api('/api/menus')[1]['read'], 6, 'menük beolvasása'); w.close()

@test
def t_menu_sweep():
    """Beolvas minden menüt, a resetet nem érinti"""
    with Stack() as s:
        sweep(s); j = api('/api/menus')[1]
        assert len(j['defs']) == 159 and len(j['vals']) >= 150, len(j['vals'])
        assert not [c for c in s.cmds() if c.startswith('EX1701')]

@test
def t_menu_write_backup():
    """Írás előtt mentés készül, az írást visszaellenőrzi"""
    with Stack() as s:
        sweep(s); old = api('/api/menus')[1]['vals']['1603']
        assert api('/api/write', '1603=040\n0513=-05\n')[0] == 200
        wait(lambda: s.rig()['ex']['1603'] == '040' and s.rig()['ex']['0513'] == '-05', 3, 'írás')
        wait(lambda: 'kiírva' in state()['notice'], 2, 'jelzés')
        b = api('/api/backups')[1]; assert len(b) == 1 and b[0]['items']['1603'] == old
        c = s.cmds(); assert c.index('EX1603040;') > 0
        assert api('/api/apply?type=backup&name=' + b[0]['name'], '')[0] == 200
        wait(lambda: s.rig()['ex']['1603'] == old and s.rig()['ex']['0513'] == '+00', 3, 'visszaállítás')

@test
def t_menu_protected():
    """Védett és hibás értéket nem ír a rádióba"""
    with Stack() as s:
        sweep(s); n = len(s.cmds())
        api('/api/write', '1701=0\n0506=0\n0508=1\n1801=9999\n1603=999\n1603=40\n0401=9\nxxxx=1\n')
        wait(lambda: 'kihagytam' in state()['notice'] or 'Nincs' in state()['notice'], 3, 'jelzés')
        bad = [c for c in s.cmds()[n:] if c.startswith('EX') and len(c) > 7]
        assert not bad, bad; assert not api('/api/backups')[1]

@test
def t_menu_old_table():
    """Eltérő menütáblánál nem ír a menükbe"""
    with Stack('--old-menu') as s:
        api('/api/write', '1603=040\n'); time.sleep(.5)
        assert not [c for c in s.cmds() if c.startswith('EX') and len(c) > 7]
        wait(lambda: 'nem ír' in state()['notice'], 2, 'jelzés')
        assert api('/api/write', '1603=040\n')[0] == 200, 'beragadt a tiltás'

@test
def t_write_during_tx():
    """Adás közben nem ír a menükbe"""
    with Stack() as s:
        sweep(s); k, _ = rigctl(['T 1'], keep=True); wait(lambda: s.rig()['tx'] == 1, 1, 'adás')
        n = len(s.cmds()); api('/api/write', '1603=040\n'); time.sleep(.6)
        assert not [c for c in s.cmds()[n:] if c.startswith('EX1603')], 'írt adás közben'
        k.close()

@test
def t_profiles():
    """Profil mentése, alkalmazása, törlése, biztonságos névvel"""
    with Stack() as s:
        sweep(s)
        names = [p['name'] for p in api('/api/profiles')[1]]; assert set(names) == {'SSB', 'Digi', 'CW'}
        w = WS(); api('/api/apply?type=profile&name=Digi', '')
        wait(lambda: 'Digi' in state()['notice'], 3, 'Digi kész'); assert state()['mode'] == 'DATA-U'
        wait(lambda: s.rig()['ex']['0809'] == '1', 1, 'Digi menü')
        api('/api/apply?type=profile&name=SSB', ''); wait(lambda: state()['mode'] == 'USB' and 'SSB' in state()['notice'], 3, 'SSB mód')
        assert api('/api/profile?name=Teszt%20%C3%A1', '1603=033\n')[0] == 200
        api('/api/apply?type=profile&name=Teszt%20%C3%A1', ''); wait(lambda: s.rig()['ex']['1603'] == '033', 3, 'saját profil')
        assert api('/api/profile-del?name=Teszt%20%C3%A1', '')[0] == 200
        assert api('/api/profile?name=..%2F..%2Fx', 'a=1')[0] == 409
        assert api('/api/apply?type=profile&name=..%2Fconfig', '')[0] == 409
        w.close()

@test
def t_memory():
    """Memória olvasása, írása visszaellenőrzéssel, hibás adat elutasítása"""
    with Stack() as s:
        w = WS(); w.send('memread'); wait(lambda: api('/api/mem')[1]['read'], 8, 'memóriák')
        r = api('/api/mem')[1]['rows']; assert len(r) == 117 and r[0]['tag'] == 'FT8 20M' and r[0]['md'] == 'C' and not r[5]['on']
        assert r[99]['ch'] == 'P1L' and r[116]['ch'] == 'P9U'
        assert api('/api/mem-write', '005|3573000|8|0|0|0|0|80M DIGI\n')[0] == 200
        wait(lambda: s.rig()['mem'].get('005', {}).get('tag') == '80M DIGI', 3, 'írás')
        wait(lambda: api('/api/mem')[1]['rows'][4]['on'] == 1, 2, 'visszaolvasás')
        assert api('/api/mem-write', '005|99|2|0|0|0|0|\n')[0] == 409
        assert api('/api/mem-write', '005|7000000|2|0|0|0|0|a;b\n')[0] == 409
        assert api('/api/mem-write', '005|7000000|2|0|0|0|0|tizenharomkar\n')[0] == 409
        assert api('/api/mem-write', '999|7000000|2|0|0|0|0|\n')[0] == 409
        w.send('memrecall 0'); wait(lambda: state()['fa'] == 14074000 and state()['mode'].startswith('DATA'), 2, 'előhívás')
        w.close()

@test
def t_data_auth():
    """Az adat-API PIN és saját eredet nélkül nem érhető el"""
    with Stack() as s:
        h = {'Host': 'evil.example:18891'}
        for u in ('/api/menus', '/api/mem', '/api/profiles', '/api/backups'):
            assert http_req('GET', u, headers=h)[0] == 401
        assert http_req('POST', '/api/write', '1603=040', headers=h)[0] == 401
        assert http_req('POST', '/api/write', '1603=040', headers={'Origin': 'http://evil.example'})[0] == 403

FAKE = """#!/usr/bin/env python3
import math, os, struct, sys, time
a = sys.argv
if '-L' in a:
    print('null\\n    Discard\\ndefault\\n    Alapértelmezett\\nhw:CARD=Device,DEV=0\\n    USB nyers\\nplughw:CARD=Device,DEV=0\\n    USB Audio Device\\nplughw:CARD=BUSY,DEV=0\\n    Foglalt\\ndsnoop:CARD=Device,DEV=0\\n    USB megosztott')
    sys.exit(0)
open(os.environ['FAKE_LOG'], 'a').write(' '.join(a[1:]) + '\\n')
if 'BUSY' in a[a.index('-D') + 1]:
    print('arecord: main: audio open error: Device or resource busy', file=sys.stderr); sys.exit(1)
ch = int(a[a.index('-c') + 1]); r = 48000; n = 0
while True:
    b = b''.join(struct.pack('<h', int(9000 * math.sin(2 * math.pi * 1000 * (n + i) / r))) * ch for i in range(2400))
    sys.stdout.buffer.write(b); sys.stdout.buffer.flush(); n += 2400; time.sleep(.05)
"""

def fake_arecord():
    d = tempfile.mkdtemp(prefix='ft891a'); p = d + '/arecord'
    open(p, 'w').write(FAKE); os.chmod(p, 0o755); os.environ['FAKE_LOG'] = d + '/log'
    return p

@test
def t_waterfall():
    """A vízesés a hangkártya hangját a helyes frekvencián mutatja"""
    with Stack(ctl=('--arecord', fake_arecord())) as s:
        names = [d['name'] for d in api('/api/audio')[1]]
        assert 'plughw:CARD=Device,DEV=0' in names and 'dsnoop:CARD=Device,DEV=0' in names and 'null' not in names and not [n for n in names if n.startswith('hw:')], names
        w = WS(); w.send('wfdev nincsilyen'); wait(lambda: 'Nincs ilyen' in state()['notice'], 1, 'tiltott eszköz')
        w.send('wfdev plughw:CARD=Device,DEV=0'); w.send('wf 1')
        fr = [d[1:] for o, d in w.frames(2.5) if o == 2 and d[0] == 1]; assert len(fr) >= 5, len(fr)
        assert len(fr[-1]) == 512; peak = max(range(512), key=lambda i: fr[-1][i])
        assert abs((peak + 1) * 48000 / 8192 - 1000) < 12, peak
        lg = open(os.environ['FAKE_LOG']).read(); assert lg.count('-D plughw') == 1, lg
        w.send('wfdev plughw:CARD=BUSY,DEV=0')
        wait(lambda: 'másik program' in state()['wf_status'], 4, 'foglalt hangkártya')
        w.close(); time.sleep(.5)
        assert not running('arecord'), 'futva maradt'

APLAY = """#!/usr/bin/env python3
import os, sys
open(os.environ['FAKE_APLAY'] + '.args', 'a').write(' '.join(sys.argv[1:]) + '\\n')
with open(os.environ['FAKE_APLAY'], 'ab', buffering=0) as f:
    while True:
        d = sys.stdin.buffer.read1(4096)
        if not d: break
        f.write(d)
"""

def fake_aplay():
    d = tempfile.mkdtemp(prefix='ft891p'); p = d + '/aplay'
    open(p, 'w').write(APLAY); os.chmod(p, 0o755); os.environ['FAKE_APLAY'] = d + '/out'
    return p

def mu_dec(i):
    u = ~i & 255; e = (u >> 4) & 7; x = (((u & 15) << 3) + 132 << e) - 132
    return -x if u & 128 else x

def tone_power(b, hz):
    re = im = 0
    for i, v in enumerate(b):
        re += v * math.cos(2 * math.pi * hz * i / 8000); im += v * math.sin(2 * math.pi * hz * i / 8000)
    return re * re + im * im

@test
def t_rx_audio():
    """A vételi hang 8 kHz-en, a helyes hangmagassággal jut el, adás alatt néma"""
    with Stack(ctl=('--arecord', fake_arecord())) as s:
        w = WS(); w.send('wfdev plughw:CARD=Device,DEV=0'); w.send('rx 1')
        fr = [d for o, d in w.frames(2.5) if o == 2 and d[0] == 2]
        assert len(fr) >= 10 and len(fr[-1]) == 641, len(fr)
        sm = [mu_dec(x) for x in fr[-1][1:]]
        assert tone_power(sm, 1000) > 100 * tone_power(sm, 2300)
        assert 0.15 < max(abs(x) for x in sm) / 32768 < 0.4, max(sm)
        w.send('ptt 1'); wait(lambda: s.rig()['tx'] == 1, 1, 'adás'); time.sleep(.3); w.frames(.2)
        w.send('ptt 1'); n = [1 for o, d in w.frames(.6) if o == 2 and d[0] == 2]
        assert len(n) <= 1, len(n); w.close()

@test
def t_tx_audio():
    """A mikrofon hangja csak adás alatt megy a hangkártyára, utána leáll"""
    with Stack(ctl=('--arecord', fake_arecord(), '--aplay', fake_aplay())) as s:
        out = os.environ['FAKE_APLAY']; size = lambda: os.path.getsize(out) if os.path.exists(out) else 0
        w = WS(); w.send('wfdev dsnoop:CARD=Device,DEV=0'); time.sleep(.3)
        w.send('txa 1'); time.sleep(.4); w.send_bin(b'\x55' * 320); time.sleep(.3)
        assert size() == 0, 'adás nélkül is ment'
        w.send('ptt 1'); wait(lambda: s.rig()['tx'] == 1, 1, 'adás')
        for _ in range(5): w.send_bin(b'\x55' * 320); w.send('ptt 1'); time.sleep(.04)
        wait(lambda: size() == 1600, 2, 'hang a kártyán')
        assert 'dmix:CARD=Device,DEV=0' in open(out + '.args').read() and 'MU_LAW' in open(out + '.args').read()
        w.send('ptt 0'); w.send('txa 0'); wait(lambda: s.rig()['tx'] == 0, 1, 'adás ki'); time.sleep(.2)
        assert not running('aplay'), 'aplay futva maradt'
        w.send('txa 1'); time.sleep(.3); w.close(); time.sleep(.4)
        assert not running('aplay'), 'aplay a kilépés után is fut'

@test
def t_tls():
    """HTTPS-en az oldal, a PIN és a WebSocket is működik"""
    with Stack() as s:
        crt = s.d + '/cfg/tls-cert.pem'; assert os.path.exists(crt)
        assert oct(os.stat(s.d + '/cfg/tls-key.pem').st_mode)[-3:] == '600'
        assert b'IP Address:127.0.0.1' in subprocess.check_output(['openssl', 'x509', '-in', crt, '-noout', '-ext', 'subjectAltName'])
        ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        c = http.client.HTTPSConnection('127.0.0.1', WEB + 1, context=ctx, timeout=3)
        c.request('POST', '/api/login', 'pin=' + s.pin(), {'Host': 'radio.example:18892', 'Origin': 'https://radio.example:18892'})
        r = c.getresponse(); r.read(); assert r.status == 200; ck = r.getheader('Set-Cookie').split(';')[0]
        w = WS(tls=True, headers=f'Cookie: {ck}\r\n'); assert w.code == 101
        fr = w.frames(.6); assert fr and json.loads(fr[0][1])['t'] == 's'; w.send('fa 7100000')
        wait(lambda: s.rig()['fa'] == 7100000, 1, 'parancs TLS-en'); w.close()
        k = socket.create_connection(('127.0.0.1', WEB + 1), timeout=2); k.sendall(b'GET / HTTP/1.0\r\n\r\n'); k.settimeout(1)
        try: assert b'FT-891' not in k.recv(100), 'titkosítás nélkül is válaszolt'
        except ConnectionError: pass
        k.close()

@test
def t_power():
    """Be- és kikapcsolás CAT-en, adás közben tiltva"""
    with Stack('--off', nowait=True) as s:
        assert not s.rig()['power'] and not state()['connected']
        w = WS(); time.sleep(.2); w.send('power 1')
        wait(lambda: s.rig()['power'], 6, 'bekapcsolás'); wait(lambda: state()['connected'], 8, 'kapcsolódás')
        k, _ = rigctl(['T 1'], keep=True); wait(lambda: s.rig()['tx'] == 1, 1, 'adás')
        w.send('power 0'); time.sleep(.5); assert s.rig()['power'], 'adás közben kikapcsolt'; k.close()
        wait(lambda: s.rig()['tx'] == 0, 1, 'adás ki'); w.send('power 0')
        wait(lambda: not s.rig()['power'], 2, 'kikapcsolás')
        wait(lambda: not state()['connected'] and 'kikapcsolva' in state()['status'], 5, 'jelzés'); w.close()

SSTV_FAKE = """#!/usr/bin/env python3
import os, sys, time
if '-L' in sys.argv:
    print('plughw:CARD=Device,DEV=0\\n    USB Audio Device'); sys.exit(0)
d = open(os.environ['FAKE_RAW'], 'rb').read()
if '-c' in sys.argv and sys.argv[sys.argv.index('-c') + 1] != '1': sys.exit(1)
for i in range(0, len(d), 96000):
    sys.stdout.buffer.write(d[i:i + 96000]); sys.stdout.buffer.flush(); time.sleep(.01)
while True:
    sys.stdout.buffer.write(bytes(4800)); sys.stdout.buffer.flush(); time.sleep(.1)
"""

def read_png(b):
    assert b[:8] == b'\x89PNG\r\n\x1a\n'
    o = 8; z = b''; w = h = 0
    while o < len(b):
        n = int.from_bytes(b[o:o + 4], 'big'); t = b[o + 4:o + 8]; d = b[o + 8:o + 8 + n]
        assert zlib.crc32(b[o + 4:o + 8 + n]) == int.from_bytes(b[o + 8 + n:o + 12 + n], 'big'), 'CRC'
        if t == b'IHDR': w = int.from_bytes(d[:4], 'big'); h = int.from_bytes(d[4:8], 'big')
        if t == b'IDAT': z += d
        o += 12 + n
    r = zlib.decompress(z); row = w * 3 + 1
    return w, h, b''.join(r[i * row + 1:(i + 1) * row] for i in range(h))

def sstv_case(mode, *gen, limit=30.0, go=None):
    d = tempfile.mkdtemp(prefix='ft891s'); raw, rgb = d + '/a.raw', d + '/a.rgb'
    subprocess.check_call([sys.executable, ROOT + '/tools/sstvgen.py', mode, raw, rgb, *gen])
    f = d + '/arecord'; open(f, 'w').write(SSTV_FAKE); os.chmod(f, 0o755); os.environ['FAKE_RAW'] = raw
    with Stack(ctl=('--arecord', f)) as s:
        w = WS(); w.send('wfdev plughw:CARD=Device,DEV=0'); w.send('sstv 1')
        if go is not None: time.sleep(.1); w.send(f'sstvgo {go}')
        wait(lambda: api('/api/sstv')[1]['images'], 30, 'kész kép')
        n = api('/api/sstv')[1]['images'][0]
        st, b, h = http_req('GET', '/api/sstv-img?name=' + n); assert st == 200 and h['Content-Type'] == 'image/png'
        W, H, px = read_png(b); ref = open(rgb, 'rb').read()
        assert len(px) == len(ref), (W, H)
        if go is None:
            e = [abs(px[i] - ref[i]) for i in range(0, len(ref), 7)]
            m = sum(e) / len(e)
        else:
            # Kézi indításnál a kép bármelyik sortól kezdődhet: a legjobb függőleges illesztést nézzük
            row = W * 3; m = 1e9
            for y0 in range(2, 80, 8):
                for sh in range(0, H - 42, 6):
                    e = [abs(px[row * (y + y0) + x] - ref[row * (y + sh) + x]) for y in range(0, 40, 8) for x in range(0, row, 23)]
                    m = min(m, sum(e) / len(e))
            if os.environ.get('SSTV_DUMP'): open(os.environ['SSTV_DUMP'], 'wb').write(b)
        w.close()
    print(f'           {mode} {" ".join(gen)}: átlagos eltérés {m:.1f}/255')
    assert m < limit, m
    return m

@test
def t_sstv_modes():
    """SSTV: minden főbb mód felismerése VIS-ből és dekódolása"""
    for md in ('S1', 'S2', 'M1', 'M2', 'R36', 'R72', 'PD50', 'PD120'): sstv_case(md)

@test
def t_sstv_real():
    """SSTV: zaj, elhangolás és hangkártya-órajel eltérése mellett"""
    sstv_case('S2', '--noise', '0.05')
    sstv_case('S2', '--ofs', '60')
    sstv_case('S2', '--ppm', '400')
    sstv_case('S2', '--ppm', '-400', '--ofs', '-40', '--noise', '0.05')
    sstv_case('M1', '--ppm', '300', '--noise', '0.05')

@test
def t_sstv_manual():
    """SSTV: kézi indítás VIS nélkül"""
    sstv_case('S2', '--no-vis', '--ppm', '200', limit=45, go=1)

@test
def t_sstv_ui_api():
    """SSTV: jogosultság, tiltott fájlnév, élő sorok a felületre"""
    with Stack() as s:
        assert http_req('GET', '/api/sstv', headers={'Host': 'evil.example:18891'})[0] == 401
        assert http_req('GET', '/api/sstv-img?name=..%2Fconfig')[0] == 404
        assert http_req('GET', '/api/sstv-img?name=x.txt')[0] == 404
        assert 'Scottie 2' in api('/api/sstv')[1]['modes']

@test
def t_filters():
    """IF shift, notch, contour, APF eljut a rádióig és visszajön"""
    with Stack() as s:
        w = WS()
        for c in ['filt 1 -240', 'filt 0 1', 'filt 3 1230', 'filt 2 1', 'filt 5 800', 'filt 4 1', 'filt 7 100', 'filt 6 1', 'fn 7 1']: w.send(c)
        def ok():
            j = state(); return (j['ifs_on'] == 1 and j['ifs'] == -240 and j['notch_on'] == 1 and j['notch_hz'] == 1230 and
                                 j['cont_on'] == 1 and j['cont'] == 800 and j['apf_on'] == 1 and j['apf'] == 100 and j['eq'] == 1)
        wait(ok, 6, 'szűrők')
        r = s.rig(); assert r['isv'] == '1-0240' and r['bp'] == {'0': '001', '1': '123'} and r['co']['3'] == '0035'
        assert state()['pitch'] == 700; w.close()

@test
def t_logger_api():
    """A logolóprogramok frekvenciát és módot kapnak"""
    with Stack() as s:
        j = api('/api/rig')[1]; assert j['freq'] == 14074000 and j['mode'] == 'USB' and j['adif_mode'] == 'SSB' and j['tx'] == 0, j
        assert http_req('GET', '/api/rig', headers={'Host': 'evil.example:18891'})[0] == 401

@test
def t_sigterm_keyed():
    """Leállításkor adásban hagyott rádiót elenged"""
    s = Stack()
    try:
        k, _ = rigctl(['T 1'], keep=True); wait(lambda: s.rig()['tx'] == 1, 1, 'adás be')
        s.ctl.terminate(); s.ctl.wait(3)
        wait(lambda: s.rig()['tx'] == 0 and s.rig()['ai'] == 0, 1.5, 'elengedés')
        k.close()
    finally:
        s.close()

bad = [r for r in results if not r[1]]
print(f'\n{len(results)-len(bad)}/{len(results)} teszt sikeres')
sys.exit(1 if bad else 0)
