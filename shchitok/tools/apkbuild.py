"""Minimal APK toolchain (no Android SDK): binary XML, resources.arsc, zip with alignment, v1+v2 signing."""
import struct, zlib, hashlib, base64, io

# ---------- string pool ----------
def _len8(n):
    return bytes([n]) if n < 128 else bytes([0x80 | (n >> 8), n & 0xFF])

def string_pool(strings, utf8):
    data = bytearray(); offs = []
    for s in strings:
        offs.append(len(data))
        if utf8:
            b = s.encode('utf-8')
            data += _len8(len(s)) + _len8(len(b)) + b + b'\0'
        else:
            u = s.encode('utf-16le'); n = len(u) // 2
            assert n < 0x8000
            data += struct.pack('<H', n) + u + b'\0\0'
    while len(data) % 4: data += b'\0'
    start = 28 + 4 * len(strings)
    size = start + len(data)
    hdr = struct.pack('<HHIIIIII', 0x0001, 28, size, len(strings), 0, 0x100 if utf8 else 0, start, 0)
    return hdr + b''.join(struct.pack('<I', o) for o in offs) + bytes(data)

# ---------- binary XML ----------
ATTR_IDS = {
    'theme': 0x01010000, 'label': 0x01010001, 'icon': 0x01010002, 'name': 0x01010003,
    'exported': 0x01010010, 'configChanges': 0x0101001f, 'minSdkVersion': 0x0101020c,
    'versionCode': 0x0101021b, 'versionName': 0x0101021c, 'windowSoftInputMode': 0x0101022b,
    'targetSdkVersion': 0x01010270, 'allowBackup': 0x01010280, 'drawable': 0x01010199,
    'launchMode': 0x0101001d,
}
NS_URI = 'http://schemas.android.com/apk/res/android'
T_REF, T_STR, T_INT, T_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12

class N:
    def __init__(self, name, attrs=(), children=()):
        self.name = name; self.attrs = list(attrs); self.children = list(children)
# attr tuple: (is_android_ns, name, type, value)

def build_axml(root, utf8):
    # collect strings
    idnames = sorted({a[1] for n in _walk(root) for a in n.attrs if a[0]}, key=lambda x: ATTR_IDS[x])
    others = set(['android', NS_URI])
    for n in _walk(root):
        others.add(n.name)
        for ns, nm, ty, v in n.attrs:
            if not ns: others.add(nm)
            if ty == T_STR: others.add(v)
    others -= set(idnames)
    strings = idnames + sorted(others)
    idx = {s: i for i, s in enumerate(strings)}
    NONE = 0xFFFFFFFF
    body = bytearray()
    line = [1]
    def ln():
        line[0] += 1; return line[0]
    body += struct.pack('<HHIIIII', 0x0100, 16, 24, 1, NONE, idx['android'], idx[NS_URI])
    def emit(n):
        l = ln()
        attrs = [a for a in n.attrs if a[0]]; attrs.sort(key=lambda a: ATTR_IDS[a[1]])
        attrs += sorted([a for a in n.attrs if not a[0]], key=lambda a: a[1])
        b = struct.pack('<HHIIIIIHHHHHH', 0x0102, 16, 36 + 20 * len(attrs), l, NONE, NONE, idx[n.name], 20, 20, len(attrs), 0, 0, 0)
        for ns, nm, ty, v in attrs:
            if ty == T_STR: raw = idx[v]; data = idx[v]
            else: raw = NONE; data = v & 0xFFFFFFFF
            b += struct.pack('<IIIHBBI', idx[NS_URI] if ns else NONE, idx[nm], raw, 8, 0, ty, data)
        body.extend(b)
        for c in n.children: emit(c)
        body.extend(struct.pack('<HHIIIII', 0x0103, 16, 24, ln(), NONE, NONE, idx[n.name]))
    emit(root)
    body += struct.pack('<HHIIIII', 0x0101, 16, 24, ln(), NONE, idx['android'], idx[NS_URI])
    resmap = struct.pack('<HHI', 0x0180, 8, 8 + 4 * len(idnames)) + b''.join(struct.pack('<I', ATTR_IDS[x]) for x in idnames)
    pool = string_pool(strings, utf8)
    total = 8 + len(pool) + len(resmap) + len(body)
    return struct.pack('<HHI', 0x0003, 8, total) + pool + resmap + bytes(body)

def _walk(n):
    yield n
    for c in n.children: yield from _walk(c)

# ---------- resources.arsc ----------
def build_arsc(package_name, types):
    """types: list of (type_name, [ (entry_name, [ (config_bytes64, path) ... ]) ... ], spec_flags) ; one entry per type supported per list."""
    gpaths = []
    for _, entries, _ in types:
        for _, cfgs in entries:
            for _, p in cfgs:
                if p not in gpaths: gpaths.append(p)
    gpool = string_pool(gpaths, True)
    tpool = string_pool([t[0] for t in types], False)
    keys = []
    for _, entries, _ in types:
        for en, _ in entries:
            if en not in keys: keys.append(en)
    kpool = string_pool(keys, True)
    chunks = bytearray()
    for ti, (tname, entries, flags) in enumerate(types):
        tid = ti + 1
        n_entries = len(entries)
        chunks += struct.pack('<HHIBBHI', 0x0202, 16, 16 + 4 * n_entries, tid, 0, 0, n_entries)
        chunks += b''.join(struct.pack('<I', flags) for _ in entries)
        # one Type chunk per distinct config (assume all entries share the same config list)
        ncfg = len(entries[0][1])
        for ci in range(ncfg):
            cfg = entries[0][1][ci][0]
            ents = bytearray(); offs = []
            for en, cfgs in entries:
                offs.append(len(ents))
                ents += struct.pack('<HHI', 8, 0, keys.index(en)) + struct.pack('<HBBI', 8, 0, 0x03, gpaths.index(cfgs[ci][1]))
            start = 84 + 4 * n_entries
            size = start + len(ents)
            chunks += struct.pack('<HHIBBHII', 0x0201, 84, size, tid, 0, 0, n_entries, start) + cfg
            chunks += b''.join(struct.pack('<I', o) for o in offs) + bytes(ents)
    name16 = package_name.encode('utf-16le').ljust(256, b'\0')
    ph = 288
    tp_off = ph
    kp_off = ph + len(tpool)
    pkg_size = ph + len(tpool) + len(kpool) + len(chunks)
    pkg = struct.pack('<HHII', 0x0200, ph, pkg_size, 0x7f) + name16 + struct.pack('<IIIII', tp_off, 0, kp_off, 0, 0)
    assert len(pkg) == ph, len(pkg)
    pkg += tpool + kpool + bytes(chunks)
    total = 12 + len(gpool) + len(pkg)
    return struct.pack('<HHII', 0x0002, 12, total, 1) + gpool + pkg

# ---------- zip ----------
def write_zip(entries, date=(2026, 10, 17)):
    """entries: list of (name, bytes, 'stored'|'deflate'); data of stored entries is 4-byte aligned."""
    out = bytearray(); cd = bytearray()
    dosdate = ((date[0] - 1980) << 9) | (date[1] << 5) | date[2]
    for name, data, method in entries:
        nameb = name.encode('utf-8'); crc = zlib.crc32(data) & 0xFFFFFFFF
        if method == 'deflate':
            co = zlib.compressobj(9, zlib.DEFLATED, -15); comp = co.compress(data) + co.flush(); m = 8
            if len(comp) >= len(data): comp = data; m = 0
        else:
            comp = data; m = 0
        off = len(out); extra = b''
        if m == 0: extra = b'\0' * ((-(off + 30 + len(nameb))) % 4)
        flags = 0x0800 if any(ord(c) > 127 for c in name) else 0
        out += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, m, 0, dosdate, crc, len(comp), len(data), len(nameb), len(extra))
        out += nameb + extra + comp
        cd += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 0x031e, 20, flags, m, 0, dosdate, crc, len(comp), len(data), len(nameb), 0, 0, 0, 0, 0x81a40000, off) + nameb
    cd_off = len(out)
    out += cd
    out += struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(cd), cd_off, 0)
    return bytes(out)

def read_zip(data):
    """-> list of (name, uncompressed bytes) in file order, via python zipfile."""
    import zipfile
    z = zipfile.ZipFile(io.BytesIO(data))
    return [(i.filename, z.read(i.filename)) for i in z.infolist()]

# ---------- signing ----------
def _b64(b): return base64.b64encode(b).decode()

def v1_files(entries, cert, key, created_by='shchitok-build'):
    """entries: list of (name, bytes). returns [(name, bytes)] META-INF files (SF, RSA, MF)."""
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.serialization import pkcs7
    mf = 'Manifest-Version: 1.0\r\nCreated-By: %s\r\n\r\n' % created_by
    sections = []
    for name, data in entries:
        if name.startswith('META-INF/'): continue
        sec = 'Name: %s\r\nSHA-256-Digest: %s\r\n\r\n' % (name, _b64(hashlib.sha256(data).digest()))
        sections.append((name, sec)); mf += sec
    mfb = mf.encode()
    sf = 'Signature-Version: 1.0\r\nSHA-256-Digest-Manifest: %s\r\nCreated-By: %s\r\nX-Android-APK-Signed: 2\r\n\r\n' % (_b64(hashlib.sha256(mfb).digest()), created_by)
    for name, sec in sections:
        sf += 'Name: %s\r\nSHA-256-Digest: %s\r\n\r\n' % (name, _b64(hashlib.sha256(sec.encode()).digest()))
    sfb = sf.encode()
    rsa = pkcs7.PKCS7SignatureBuilder().set_data(sfb).add_signer(cert, key, hashes.SHA256()).sign(
        serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.Binary, pkcs7.PKCS7Options.NoAttributes])
    return [('META-INF/MANIFEST.MF', mfb), ('META-INF/CERT.SF', sfb), ('META-INF/CERT.RSA', rsa)]

def _lp(b): return struct.pack('<I', len(b)) + b

def _chunk_digests(buf):
    out = []
    for i in range(0, max(len(buf), 1), 1 << 20):
        c = buf[i:i + (1 << 20)]
        out.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return out

def content_digest(sec1, sec3, sec4):
    ds = _chunk_digests(sec1) + _chunk_digests(sec3) + _chunk_digests(sec4)
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(ds)) + b''.join(ds)).digest()

def split_zip(apk):
    eocd = apk.rfind(b'PK\x05\x06')
    cd_size, cd_off = struct.unpack_from('<II', apk, eocd + 12)
    return apk[:cd_off], apk[cd_off:cd_off + cd_size], apk[eocd:], cd_off, eocd

def v2_sign(apk, cert, key):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    sec1, sec3, sec4, cd_off, eocd = split_zip(apk)
    sec4 = bytearray(sec4); struct.pack_into('<I', sec4, 16, cd_off)   # offset of CD == start of signing block
    digest = content_digest(sec1, sec3, bytes(sec4))
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    signed = _lp(_lp(struct.pack('<I', 0x0103) + _lp(digest))) + _lp(_lp(cert_der)) + _lp(b'')
    sig = key.sign(signed, padding.PKCS1v15(), hashes.SHA256())
    sigs = _lp(_lp(struct.pack('<I', 0x0103) + _lp(sig)))
    pub = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = _lp(_lp(signed) + sigs + _lp(pub))
    value = _lp(signer)
    pair = struct.pack('<QI', len(value) + 4, 0x7109871a) + value
    block_size = len(pair) + 8 + 16
    block = struct.pack('<Q', block_size) + pair + struct.pack('<Q', block_size) + b'APK Sig Block 42'
    new_eocd = bytearray(apk[eocd:]); struct.pack_into('<I', new_eocd, 16, cd_off + len(block))
    return apk[:cd_off] + block + sec3 + bytes(new_eocd)

def v2_verify(apk):
    """Independent re-implementation of v2 verification; returns True/raises."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    eocd = apk.rfind(b'PK\x05\x06')
    cd_size, cd_off = struct.unpack_from('<II', apk, eocd + 12)
    assert apk[cd_off - 16:cd_off] == b'APK Sig Block 42', 'no block'
    bsize = struct.unpack_from('<Q', apk, cd_off - 24)[0]
    bstart = cd_off - bsize - 8
    assert struct.unpack_from('<Q', apk, bstart)[0] == bsize
    p = bstart + 8; end = cd_off - 24
    while p < end:
        l, i = struct.unpack_from('<QI', apk, p)
        if i == 0x7109871a: val = apk[p + 12:p + 8 + l]; break
        p += 8 + l
    else: raise AssertionError('no v2 id')
    def rd(b, o):
        n = struct.unpack_from('<I', b, o)[0]; return b[o + 4:o + 4 + n], o + 4 + n
    signers, _ = rd(val, 0)
    signer, _ = rd(signers, 0)
    signed, o = rd(signer, 0); sigs, o = rd(signer, o); pub, o = rd(signer, o)
    sd, o2 = rd(signed, 0)
    dig, _ = rd(sd, 0); algo = struct.unpack_from('<I', dig, 4)[0]; digest, _ = rd(dig, 4 + 4 - 4 + 4 - 4) if False else (None, None)
    # parse digest entry: [len][algo u32][len digest]
    e, _ = rd(sd, 0); algo = struct.unpack_from('<I', e, 0)[0]; digest, _ = rd(e, 4)
    certs, o2 = rd(signed, o2); cert_der, _ = rd(certs, 0)
    sg, _ = rd(sigs, 0); salgo = struct.unpack_from('<I', sg, 0)[0]; sig, _ = rd(sg, 4)
    assert algo == salgo == 0x0103, hex(algo)
    cert = x509.load_der_x509_certificate(cert_der)
    cert.public_key().verify(sig, signed, padding.PKCS1v15(), hashes.SHA256())
    sec1 = apk[:bstart]; sec3 = apk[cd_off:cd_off + cd_size]
    sec4 = bytearray(apk[eocd:]); struct.pack_into('<I', sec4, 16, bstart)
    assert content_digest(sec1, sec3, bytes(sec4)) == digest, 'digest mismatch'
    return True
