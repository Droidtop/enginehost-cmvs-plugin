#!/usr/bin/env python3
"""
src/scheme.c, read out of GARbro's own scheme database.

    tools/read_garbro_schemes.py <GARbro>/GameData/Formats.dat src/scheme.c

A CPZ archive carries nothing that says which encryption scheme opens it, so
the engine tries every scheme it knows and keeps the one whose index checksum
verifies. That makes the scheme list the one thing standing between this engine
and a CMVS game it has never seen, and a list typed in by hand is a list that
goes stale - so it is generated, the way commands.c is.

Formats.dat is "GARbroDB", a length, and a zlib stream holding a .NET
BinaryFormatter object graph. Only the records that file actually uses are
decoded here.
"""
import struct, sys, zlib

DB  = sys.argv[1] if len(sys.argv) > 1 else 'Formats.dat'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'src/scheme.c'

blob = open(DB, 'rb').read()
if blob[:8] != b'GARbroDB':
    sys.exit('%s is not a GARbro scheme database' % DB)
raw = zlib.decompress(blob[12:])

class R:
    def __init__(self, d): self.d = d; self.p = 0
    def u8(self):  v = self.d[self.p]; self.p += 1; return v
    def i32(self): v = struct.unpack_from('<i', self.d, self.p)[0]; self.p += 4; return v
    def u32(self): v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v
    def i64(self): v = struct.unpack_from('<q', self.d, self.p)[0]; self.p += 8; return v
    def f32(self): v = struct.unpack_from('<f', self.d, self.p)[0]; self.p += 4; return v
    def f64(self): v = struct.unpack_from('<d', self.d, self.p)[0]; self.p += 8; return v
    def s(self):
        n = 0; shift = 0
        while True:
            b = self.u8(); n |= (b & 0x7F) << shift; shift += 7
            if not (b & 0x80): break
        v = self.d[self.p:self.p+n].decode('utf-8', 'replace'); self.p += n; return v

PRIM = {1:'u8',2:'u8',3:'char',5:'dec',6:'f64',7:'i16',8:'i32',9:'i64',10:'i8',
        11:'f32',13:'date',14:'u16',15:'u32',16:'u64',18:'str'}

def prim(r, code):
    if code in (1, 2, 10): return r.u8()
    if code == 7:  v = struct.unpack_from('<h', r.d, r.p)[0]; r.p += 2; return v
    if code == 8:  return r.i32()
    if code == 9:  return r.i64()
    if code == 11: return r.f32()
    if code == 6:  return r.f64()
    if code == 14: v = struct.unpack_from('<H', r.d, r.p)[0]; r.p += 2; return v
    if code == 15: return r.u32()
    if code == 16: v = struct.unpack_from('<Q', r.d, r.p)[0]; r.p += 8; return v
    if code == 18: return r.s()
    if code == 13: return r.i64()
    if code == 3:
        # char: one utf-8 sequence
        start = r.p
        b = r.d[r.p]
        n = 1 if b < 0x80 else 2 if b < 0xE0 else 3 if b < 0xF0 else 4
        r.p += n
        return r.d[start:r.p].decode('utf-8', 'replace')
    raise ValueError('primitive %d' % code)

classes = {}   # object id -> (name, [members], [readers])
objects = {}   # object id -> value

class Ref:
    def __init__(self, i): self.id = i
    def __repr__(self): return 'Ref(%d)' % self.id

def read_member_types(r, count):
    kinds = [r.u8() for _ in range(count)]
    infos = []
    for k in kinds:
        if k == 0:   infos.append(('prim', r.u8()))
        elif k == 1: infos.append(('str', None))
        elif k == 2: infos.append(('object', None))
        elif k == 3: infos.append(('sysclass', r.s()))
        elif k == 4: infos.append(('class', (r.s(), r.i32())))
        elif k == 5: infos.append(('objarray', None))
        elif k == 6: infos.append(('strarray', None))
        elif k == 7: infos.append(('primarray', r.u8()))
        else: raise ValueError('binary type %d' % k)
    return infos

def read_value(r, info):
    kind, extra = info
    if kind == 'prim': return prim(r, extra)
    return read_record(r)

def read_class_body(r, name, members, infos, oid):
    vals = {}
    for m, info in zip(members, infos):
        vals[m] = read_value(r, info)
    objects[oid] = (name, vals)
    return objects[oid]

def read_record(r):
    t = r.u8()
    if t == 0:            # SerializedStreamHeader
        r.i32(); r.i32(); r.i32(); r.i32(); return read_record(r)
    if t == 1:            # ClassWithId
        oid = r.i32(); cid = r.i32()
        name, members, infos = classes[cid]
        return read_class_body(r, name, members, infos, oid)
    if t == 4 or t == 5:  # SystemClassWithMembersAndTypes / ClassWithMembersAndTypes
        oid = r.i32(); name = r.s(); n = r.i32()
        members = [r.s() for _ in range(n)]
        infos = read_member_types(r, n)
        if t == 5: r.i32()          # library id
        classes[oid] = (name, members, infos)
        return read_class_body(r, name, members, infos, oid)
    if t == 6:            # BinaryObjectString
        oid = r.i32(); v = r.s(); objects[oid] = v; return v
    if t == 7:            # BinaryArray
        oid = r.i32(); kind = r.u8(); rank = r.i32()
        lengths = [r.i32() for _ in range(rank)]
        if kind in (3, 4, 5):
            for _ in range(rank): r.i32()
        info = read_member_types(r, 1)[0]
        total = 1
        for L in lengths: total *= L
        vals = []
        i = 0
        while i < total:
            if info[0] == 'prim':
                vals.append(prim(r, info[1])); i += 1
            else:
                v = read_record(r)
                if v == 'NULL256':
                    n = r.pending_nulls; vals.extend([None] * n); i += n
                else:
                    vals.append(v); i += 1
        objects[oid] = vals; return vals
    if t == 8:            # MemberPrimitiveTyped
        return prim(r, r.u8())
    if t == 13:           # ObjectNullMultiple256
        r.pending_nulls = r.u8(); return 'NULL256'
    if t == 14:           # ObjectNullMultiple
        r.pending_nulls = r.i32(); return 'NULL256'
    if t == 9:            # MemberReference
        return Ref(r.i32())
    if t == 10: return None           # ObjectNull
    if t == 11: return 'END'
    if t == 12: r.i32(); r.s(); return read_record(r)   # BinaryLibrary
    if t == 15:           # ArraySinglePrimitive
        oid = r.i32(); n = r.i32(); code = r.u8()
        vals = [prim(r, code) for _ in range(n)]
        objects[oid] = vals; return vals
    if t in (16, 17):     # ArraySingleObject / ArraySingleString
        oid = r.i32(); n = r.i32()
        vals = []
        while len(vals) < n:
            v = read_record(r)
            if v == 'NULL256': vals.extend([None] * r.pending_nulls)
            else: vals.append(v)
        objects[oid] = vals; return vals
    if t == 21: return 'METHOD'
    raise ValueError('record %d at %d' % (t, r.p - 1))

r = R(raw)
try:
    while r.p < len(raw):
        v = read_record(r)
        if v == 'END': break
except Exception as e:
    sys.stderr.write('stopped at %d: %s\n' % (r.p, e))

def deref(v, seen=None):
    if isinstance(v, Ref):
        return objects.get(v.id)
    return v

# every CmvsScheme object, and the titles that point at it
found = {oid: val for oid, val in objects.items()
           if isinstance(val, tuple) and val[0].endswith('CmvsScheme')}
import json
out = {}
for oid, (name, vals) in found.items():
    d = {}
    for k, v in vals.items():
        w = deref(v)
        if isinstance(w, tuple):
            w = w[1].get('value__', w[1])
        d[k] = w
    out[oid] = d
schemes = out

# --- the titles: Dictionary<string, CmvsScheme>.KeyValuePairs
titles = {}
for oid, val in objects.items():
    if not (isinstance(val, tuple) and 'Dictionary`2' in val[0] and 'CmvsScheme' in val[0]):
        continue
    pairs = deref(val[1].get('KeyValuePairs'))
    if not pairs: continue
    for kv in pairs:
        kv = deref(kv)
        if not isinstance(kv, tuple): continue
        k = deref(kv[1].get('key'))
        v = kv[1].get('value')
        vid = v.id if isinstance(v, Ref) else None
        if isinstance(k, str) and vid is not None:
            titles[k] = vid


VARIANT = ['CMVS_MD5_A', 'CMVS_MD5_B', 'CMVS_MD5_CHRONO', 'CMVS_MD5_MEMORIA',
           'CMVS_MD5_NATSU', 'CMVS_MD5_AOI', 'CMVS_MD5_MIRAI']

COMMON = [0xCD90F089,0xE982B782,0xA282AB88,0xCD82718E,0x52838A83,0xA882AA82,
          0x7592648E,0xB582AB82,0xE182BF82,0xDC82A282,0x4281B782,0xED82F48E,
          0xBF82EA82,0xA282E182,0xB782DC82,0x6081E682,0xC6824181,0xA482A282,
          0xE082A982,0xF48EA482,0xBF82C182,0xA282E182,0xB582DC82,0xF481BD82]

def words(v, per=6, indent='    '):
    out = []
    for i in range(0, len(v), per):
        out.append(indent + ' '.join('0x%08Xu,' % w for w in v[i:i+per]))
    return '\n'.join(out)

extra = {}          # title -> secret name, for the ones that do not use the common one
for t, oid in titles.items():
    sec = schemes[oid]['Cpz5Secret']
    if sec != COMMON:
        extra[t] = sec

L = []
A = L.append
A('#include "scheme.h"')
A('')
A('/*')
A(' * The 24-dword taunt string every CPZ5/CPZ6 game keys its index with. It is')
A(" * cp932 text and it sits verbatim in ChronoClock's own cmvs32.exe at 0x146000,")
A(' * which is how it was confirmed rather than copied on faith.')
A(' */')
A('const uint32_t cmvs_common_secret[24] = {')
A(words(COMMON))
A('};')
A('')
names = {}
for t, sec in sorted(extra.items()):
    ident = 'secret_' + ''.join(c.lower() if c.isalnum() else '_' for c in t)
    names[t] = ident
    A('/* %s carries its own secret rather than the common one. */' % t)
    A('static const uint32_t %s[24] = {' % ident)
    A(words(sec))
    A('};')
    A('')
A('/*')
A(' * Every CMVS archive scheme in GARbro\'s own database (GameData/Formats.dat,')
A(' * "GARbroDB" + zlib + a BinaryFormatter graph; tools/read_garbro_schemes.py')
A(' * reads it). A CPZ archive says nothing about which one opens it, so cpz_open')
A(' * tries each in turn and keeps the one whose index checksum verifies - which')
A(' * is why listing them all is what makes an unseen CMVS game open unmodified,')
A(' * and why nothing here has to guess a title from a folder name.')
A(' *')
A(' * Confirmed on real data: Chrono Clock, whose twelve archives all index and')
A(' * decrypt. The other five are transcribed, not tested - no copy to test with.')
A(' */')
A('const cmvs_scheme cmvs_schemes[] = {')
order = sorted(titles, key=lambda t: (schemes[titles[t]]['Version'], t))
for t in order:
    d = schemes[titles[t]]
    secret = names.get(t, 'cmvs_common_secret')
    A('    {   /* CPZ%d */' % d['Version'])
    A('        "%s", %s, %s,' % (t, VARIANT[d['Md5Variant']], secret))
    A('        0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%02Xu, %d,'
      % (d['DecoderFactor'], d['EntryInitKey'], d['EntrySubKey'],
         d['EntryTailKey'], d['EntryKeyPos']))
    A('        0x%08Xu, 0x%08Xu, 0x%02Xu,'
      % (d['IndexSeed'], d['IndexAddend'], d['IndexSubtrahend']))
    A('        {%s},' % ', '.join('0x%08Xu' % w for w in d['DirKeyAddend']))
    A('    },')
A('    /*')
A('     * The constants GARbro falls back on for CPZ5 when no title matches, kept')
A('     * last so an unlisted CPZ5 game is tried rather than refused outright.')
A('     */')
A('    {')
A('        "CPZ5 fallback", CMVS_MD5_MIRAI, cmvs_common_secret,')
A('        0x1A743125u, 0x2547A39Eu, 0x5C29E87Bu, 0xBCu, 9,')
A('        0x2A65CB4Eu, 0x784C5962u, 0x79u,')
A('        {0u, 0x00112233u, 0u, 0x34258765u},')
A('    },')
A('};')
A('')
A('const int cmvs_scheme_count = (int) (sizeof cmvs_schemes / sizeof cmvs_schemes[0]);')
open(OUT, 'w').write('\n'.join(L) + '\n')
print('%s: %d schemes' % (OUT, len(order) + 1))
