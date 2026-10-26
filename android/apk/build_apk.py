#!/usr/bin/env python3
"""Pack libtotk.so into an installable APK, without the Android SDK (no aapt2, no Gradle, no Java).

The app is a NativeActivity (android:hasCode="false"): the runtime library is the whole app (runtime/frontend_android.c).
This writes the binary AndroidManifest.xml itself, zips it with the native libraries, and signs the APK with the APK
Signature Scheme v2 and the v1 (JAR) scheme, using a key made on first use and kept next to the output (the same key
must sign every later build, or Android refuses the update; uninstalling removes the app's data, the game folder too).

usage: build_apk.py --lib libtotk.so [--lib more.so ...] --out totk.apk [--key apk-key.pem] [--min-sdk 33] [--target-sdk 33]
       [--package dev.kitcatkode.totk] [--label "Tears of the Kingdom"] [--version-code N]
Needs the Python package 'cryptography'.
"""
import argparse, base64, hashlib, io, os, struct, sys, time, zipfile
from pathlib import Path

ANDROID_NS = 'http://schemas.android.com/apk/res/android'

# android.R.attr resource ids of the manifest attributes used here
ATTR = {
    'theme': 0x01010000, 'label': 0x01010001, 'name': 0x01010003, 'hasCode': 0x0101000c, 'debuggable': 0x0101000f,
    'exported': 0x01010010, 'launchMode': 0x0101001d, 'screenOrientation': 0x0101001e, 'configChanges': 0x0101001f,
    'value': 0x01010024, 'minSdkVersion': 0x0101020c, 'versionCode': 0x0101021b, 'versionName': 0x0101021c,
    'targetSdkVersion': 0x01010270, 'isGame': 0x010103f4, 'extractNativeLibs': 0x010104ea, 'targetActivity': 0x01010202,
}
THEME_NOTITLEBAR_FULLSCREEN = 0x01030007   # @android:style/Theme.NoTitleBar.Fullscreen
SENSOR_LANDSCAPE = 6
# orientation | keyboard | keyboardHidden | navigation | screenLayout | uiMode | screenSize | smallestScreenSize | density
CONFIG_CHANGES = 0x80 | 0x10 | 0x20 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000
STANDARD = 0   # was singleTask (2): an alias inherits the launch mode, and a singleTask one could not open a second window

# ---------- binary XML (the format aapt writes) ----------
T_REFERENCE, T_STRING, T_INT_DEC, T_INT_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12


def manifest_tree(package, label, version_code, version_name, min_sdk, target_sdk):
    """(tag, [(ns?, name, kind, value)], children); kind: 'str', 'int', 'hex', 'bool', 'ref'"""
    A = True   # attribute in the android namespace
    return ('manifest', [(False, 'package', 'str', package), (A, 'versionCode', 'int', version_code), (A, 'versionName', 'str', version_name)], [
        ('uses-sdk', [(A, 'minSdkVersion', 'int', min_sdk), (A, 'targetSdkVersion', 'int', target_sdk)], []),
        ('application', [(A, 'label', 'str', label), (A, 'hasCode', 'bool', False), (A, 'debuggable', 'bool', True),
                         (A, 'extractNativeLibs', 'bool', True), (A, 'isGame', 'bool', True), (A, 'theme', 'ref', THEME_NOTITLEBAR_FULLSCREEN)], [
            ('activity', [(A, 'name', 'str', 'android.app.NativeActivity'), (A, 'label', 'str', label), (A, 'exported', 'bool', True),
                          (A, 'launchMode', 'int', STANDARD), (A, 'screenOrientation', 'int', SENSOR_LANDSCAPE),
                          (A, 'configChanges', 'hex', CONFIG_CHANGES)], [
                ('meta-data', [(A, 'name', 'str', 'android.app.lib_name'), (A, 'value', 'str', 'totk')], []),
                ('intent-filter', [], [
                    ('action', [(A, 'name', 'str', 'android.intent.action.MAIN')], []),
                    ('category', [(A, 'name', 'str', 'android.intent.category.LAUNCHER')], []),
                ]),
            ]),
            # the second screen (runtime/bottom_screen.c): the same NativeActivity and library under another name, which the
            # game's activity starts on the presentation display; the library tells the two apart by this name
            ('activity-alias', [(A, 'name', 'str', package + '.Bottom'), (A, 'targetActivity', 'str', 'android.app.NativeActivity'),
                                (A, 'label', 'str', label), (A, 'exported', 'bool', False)], [
                ('meta-data', [(A, 'name', 'str', 'android.app.lib_name'), (A, 'value', 'str', 'totk')], []),
            ]),
        ]),
    ])


def binary_xml(tree):
    # strings: android attribute names first (they carry resource ids, in the resource map's order), then the rest
    attr_names, others = [], []
    def collect(node):
        tag, attrs, kids = node
        for ns, name, kind, value in attrs:
            if ns and name not in attr_names: attr_names.append(name)
            elif not ns and name not in others: others.append(name)
            if kind == 'str' and value not in others: others.append(value)
        if tag not in others: others.append(tag)
        for k in kids: collect(k)
    collect(tree)
    attr_names.sort(key=lambda n: ATTR[n])
    for extra in ('android', ANDROID_NS):
        if extra not in others: others.append(extra)
    strings = attr_names + [s for s in others if s not in attr_names]
    idx = {s: i for i, s in enumerate(strings)}

    def chunk(ctype, header, body):   # header: bytes after the 8-byte chunk header
        return struct.pack('<HHI', ctype, 8 + len(header), 8 + len(header) + len(body)) + header + body

    # string pool (UTF-16)
    data, offsets = b'', []
    for s in strings:
        offsets.append(len(data))
        u = s.encode('utf-16-le'); data += struct.pack('<H', len(s)) + u + b'\0\0'
    while len(data) % 4: data += b'\0'
    header_len = 28
    body = b''.join(struct.pack('<I', o) for o in offsets) + data
    pool = chunk(0x0001, struct.pack('<IIIII', len(strings), 0, 0, header_len + 4 * len(strings), 0), body)
    resmap = chunk(0x0180, b'', b''.join(struct.pack('<I', ATTR[n]) for n in attr_names))

    NONE = 0xffffffff
    out = []
    out.append(chunk(0x0100, struct.pack('<II', 1, NONE), struct.pack('<II', idx['android'], idx[ANDROID_NS])))
    def emit(node, line=[1]):
        tag, attrs, kids = node
        line[0] += 1
        recs = []
        for ns, name, kind, value in attrs:
            nsi = idx[ANDROID_NS] if ns else NONE
            raw = NONE
            if kind == 'str': dtype, d = T_STRING, idx[value]; raw = d
            elif kind == 'int': dtype, d = T_INT_DEC, value
            elif kind == 'hex': dtype, d = T_INT_HEX, value
            elif kind == 'bool': dtype, d = T_BOOL, 0xffffffff if value else 0
            elif kind == 'ref': dtype, d = T_REFERENCE, value
            else: raise ValueError(kind)
            key = ATTR[name] if ns else 0x7fffffff   # aapt order: by resource id, plain attributes last
            recs.append((key, struct.pack('<IIIHBBI', nsi, idx[name], raw, 8, 0, dtype, d & 0xffffffff)))
        recs.sort(key=lambda r: r[0])
        ext = struct.pack('<IIHHHHHH', NONE, idx[tag], 20, 20, len(recs), 0, 0, 0)
        out.append(chunk(0x0102, struct.pack('<II', line[0], NONE), ext + b''.join(r[1] for r in recs)))
        for k in kids: emit(k)
        out.append(chunk(0x0103, struct.pack('<II', line[0], NONE), struct.pack('<II', NONE, idx[tag])))
    emit(tree)
    out.append(chunk(0x0101, struct.pack('<II', 1, NONE), struct.pack('<II', idx['android'], idx[ANDROID_NS])))
    xml = pool + resmap + b''.join(out)
    return struct.pack('<HHI', 0x0003, 8, 8 + len(xml)) + xml


# ---------- signing key ----------
def load_or_make_key(path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    import datetime
    if path.is_file():
        blob = path.read_bytes()
        key = serialization.load_pem_private_key(blob, password=None)
        cert = x509.load_pem_x509_certificate(blob)
        return key, cert
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'TotK Android port (local build)')])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=365 * 30)).sign(key, hashes.SHA256()))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
                     + cert.public_bytes(serialization.Encoding.PEM))
    os.chmod(path, 0o600)
    print(f'made a new signing key: {path} (keep it: later builds must be signed with it to update the app)')
    return key, cert


def rsa_sha256(key, data):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding
    return key.sign(data, padding.PKCS1v15(), hashes.SHA256())


# ---------- v1 (JAR) signature ----------
def b64(d): return base64.b64encode(d).decode()


def v1_files(entries, key, cert):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.serialization import pkcs7
    mf = 'Manifest-Version: 1.0\r\nCreated-By: 1.0 (totk build_apk.py)\r\n\r\n'
    sections = []
    for name, data in entries:
        sec = f'Name: {name}\r\nSHA-256-Digest: {b64(hashlib.sha256(data).digest())}\r\n\r\n'
        if any(len(l.encode()) > 70 for l in sec.split('\r\n')): raise ValueError(f'name too long for one manifest line: {name}')
        sections.append((name, sec)); mf += sec
    mfb = mf.encode()
    sf = ('Signature-Version: 1.0\r\nCreated-By: 1.0 (totk build_apk.py)\r\n'
          f'SHA-256-Digest-Manifest: {b64(hashlib.sha256(mfb).digest())}\r\nX-Android-APK-Signed: 2\r\n\r\n')
    for name, sec in sections:
        sf += f'Name: {name}\r\nSHA-256-Digest: {b64(hashlib.sha256(sec.encode()).digest())}\r\n\r\n'
    sfb = sf.encode()
    sig = (pkcs7.PKCS7SignatureBuilder().set_data(sfb).add_signer(cert, key, hashes.SHA256())
           .sign(serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.NoAttributes]))
    return [('META-INF/MANIFEST.MF', mfb), ('META-INF/CERT.SF', sfb), ('META-INF/CERT.RSA', sig)]


# ---------- v2 signature (APK Signing Block) ----------
def lp(b): return struct.pack('<I', len(b)) + b
SIG_RSA_PKCS1_SHA256 = 0x0103
V2_ID = 0x7109871a


def chunk_digest(sections):
    digests = []
    for s in sections:
        for i in range(0, len(s), 1 << 20):
            c = s[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()


def v2_sign(apk, key, cert):
    from cryptography.hazmat.primitives import serialization
    eocd = apk.rfind(b'PK\x05\x06')
    if eocd < 0 or len(apk) - eocd != 22: raise ValueError('unexpected zip end record')
    cd_size, cd_off = struct.unpack_from('<II', apk, eocd + 12)
    entries, cd, end = apk[:cd_off], apk[cd_off:eocd], bytearray(apk[eocd:])
    # the digest covers the end record as if the central directory started where the signing block will be
    struct.pack_into('<I', end, 16, cd_off)
    digest = chunk_digest([entries, cd, bytes(end)])
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    signed = lp(lp(struct.pack('<I', SIG_RSA_PKCS1_SHA256) + lp(digest))) + lp(lp(cert_der)) + lp(b'')
    spki = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = lp(signed) + lp(lp(struct.pack('<I', SIG_RSA_PKCS1_SHA256) + lp(rsa_sha256(key, signed)))) + lp(spki)
    value = lp(lp(signer))
    pair = struct.pack('<QI', 4 + len(value), V2_ID) + value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'
    struct.pack_into('<I', end, 16, cd_off + len(block))
    return entries + block + cd + bytes(end)


def build(args):
    libs = [Path(p).expanduser().resolve() for p in args.lib]
    for p in libs:
        if not p.is_file(): raise SystemExit(f'no such file: {p}')
    manifest = binary_xml(manifest_tree(args.package, args.label, args.version_code, args.version_name, args.min_sdk, args.target_sdk))
    entries = [('AndroidManifest.xml', manifest)] + [(f'lib/arm64-v8a/{p.name}', p.read_bytes()) for p in libs]
    key, cert = load_or_make_key(Path(args.key).expanduser())
    entries = v1_files(entries, key, cert) + entries   # META-INF first, as jarsigner writes it
    buf = io.BytesIO()
    stamp = time.localtime()[:6]
    with zipfile.ZipFile(buf, 'w') as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, date_time=stamp); zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
    apk = v2_sign(buf.getvalue(), key, cert)
    out = Path(args.out).expanduser(); out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(apk)
    print(f'wrote {out} ({len(apk) / 2**20:.1f} MiB, package {args.package}, version {args.version_code})')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lib', action='append', required=True, help='native library for lib/arm64-v8a (libtotk.so first; repeatable)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--key', default='apk-key.pem', help='signing key + certificate (PEM); made on first use')
    ap.add_argument('--package', default='dev.kitcatkode.totk')
    ap.add_argument('--label', default='Tears of the Kingdom')
    ap.add_argument('--version-code', type=int, default=int(time.time()) // 60, help='default: minutes since 1970, so each build updates the last')
    ap.add_argument('--version-name', default='0.1')
    ap.add_argument('--min-sdk', type=int, default=33)
    ap.add_argument('--target-sdk', type=int, default=33)
    build(ap.parse_args(argv))


if __name__ == '__main__':
    main(sys.argv[1:])
