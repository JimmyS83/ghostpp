#!/usr/bin/env python3
"""Vytvori Ghost++ .cfg k mape (nebo hromadne ke vsem mapam v adresari).

Pouziti:
    python make_ghost_cfg.py cesta/k/mape.w3x
    python make_ghost_cfg.py C:\\ghost\\maps                 (vsechny mapy v adresari)
    python make_ghost_cfg.py C:\\ghost\\maps -r              (vcetne podadresaru)
    python make_ghost_cfg.py C:\\ghost\\maps -o C:\\ghost\\mapcfgs
    python make_ghost_cfg.py C:\\ghost\\maps --prefix "Maps\\Download" --overwrite
    python make_ghost_cfg.py C:\\ghost\\maps --jdir C:\\ghost\\mapcfgs

Z mapy (MPQ archiv) skript cte war3map.w3i a do cfg zapise:
  map_size, map_info, map_width, map_height, map_numplayers, map_numteams, map_slotN.
map_crc a map_sha1 se pocitaji i z common.j a blizzard.j (z Warcraftu III, ty samy
soubory, ktere pouziva Ghost++). Skript je hleda v adresari --jdir, jinak vedle
skriptu. Kdyz tam nejsou, map_crc a map_sha1 se do cfg nezapisou.

Omezeni: cteni MPQ zvlada soubory ulozene bez komprese, zlib, bzip2 a PKWARE.
Pri jine kompresi (nebo poskozene mape) se zapise cfg jen s hodnotami, ktere jdou
spocitat bez cteni mapy (map_path, map_size, map_info, map_localpath ...) a skript
vypise varovani, co v cfg chybi.
"""
import argparse
import bz2
import hashlib
import os
import struct
import sys
import zlib

EXTS = (".w3x", ".w3m")


# ----------------------------------------------------------------- MPQ reader
def _crypt_table():
    t = [0] * 1280
    seed = 0x00100001
    for i in range(256):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            t1 = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            t2 = seed & 0xFFFF
            t[idx] = t1 | t2
            idx += 256
    return t


CT = _crypt_table()
M32 = 0xFFFFFFFF


def _hash(s, kind):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().encode("latin-1", "replace"):
        s1 = CT[kind * 256 + ch] ^ ((s1 + s2) & M32)
        s2 = (ch + s1 + s2 + ((s2 << 5) & M32) + 3) & M32
    return s1


def _decrypt(data, key):
    n = len(data) // 4
    words = struct.unpack("<%dI" % n, data[: n * 4])
    out = []
    seed = 0xEEEEEEEE
    for w in words:
        seed = (seed + CT[0x400 + (key & 0xFF)]) & M32
        ch = w ^ ((key + seed) & M32)
        key = ((((~key) & M32) << 21) + 0x11111111) & M32 | (key >> 11)
        seed = (ch + seed + ((seed << 5) & M32) + 3) & M32
        out.append(ch)
    return struct.pack("<%dI" % n, *out) + data[n * 4:]


# PKWARE DCL (explode); prevod z blast.c (zlib, Mark Adler)
def _construct(rep):
    length = []
    for b in rep:
        length += [b & 15] * ((b >> 4) + 1)
    count = [0] * 14
    for l in length:
        count[l] += 1
    offs = [0] * 15
    for l in range(1, 13):
        offs[l + 1] = offs[l] + count[l]
    sym = [0] * len(length)
    for s, l in enumerate(length):
        if l:
            sym[offs[l]] = s
            offs[l] += 1
    return count, sym


_LIT = _construct(bytes([11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173]))
_LEN = _construct(bytes([2, 35, 36, 53, 38, 23]))
_DIST = _construct(bytes([2, 20, 53, 230, 247, 151, 248]))
_BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]


def pk_explode(data):
    pos = 0                      # pozice v bitech
    total = len(data) * 8

    def bits(n):
        nonlocal pos
        if pos + n > total:
            raise ValueError("PKWARE: necekany konec dat")
        v = 0
        for i in range(n):
            v |= ((data[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i
        pos += n
        return v

    def decode(table):
        nonlocal pos
        count, sym = table
        code = first = index = 0
        for ln in range(1, 14):
            if pos >= total:
                raise ValueError("PKWARE: necekany konec dat")
            code |= ((data[pos >> 3] >> (pos & 7)) & 1) ^ 1
            pos += 1
            c = count[ln]
            if code < first + c:
                return sym[index + code - first]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise ValueError("PKWARE: neplatny kod")

    lit = bits(8)
    dict_ = bits(8)
    if lit > 1 or not 4 <= dict_ <= 6:
        raise ValueError("PKWARE: neplatna hlavicka")
    out = bytearray()
    while True:
        if bits(1):
            s = decode(_LEN)
            ln = _BASE[s] + bits(_EXTRA[s])
            if ln == 519:
                break
            sh = 2 if ln == 2 else dict_
            dist = (decode(_DIST) << sh) + bits(sh) + 1
            if dist > len(out):
                raise ValueError("PKWARE: vzdalenost mimo data")
            for _ in range(ln):
                out.append(out[-dist])
        else:
            out.append(decode(_LIT) if lit else bits(8))
    return bytes(out)


class MPQ:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        base = None
        for off in range(0, len(self.d) - 32, 512):
            if self.d[off:off + 4] == b"MPQ\x1a":
                base = off
                break
        if base is None:
            raise ValueError("MPQ hlavicka nenalezena")
        self.base = base
        (_, _, _, _, shift, hpos, bpos, hn, bn) = struct.unpack_from("<4sIIHHIIII", self.d, base)
        self.ss = 512 << shift
        h = _decrypt(self.d[base + hpos: base + hpos + hn * 16], _hash("(hash table)", 3))
        b = _decrypt(self.d[base + bpos: base + bpos + bn * 16], _hash("(block table)", 3))
        self.hash = [struct.unpack_from("<IIHHI", h, i * 16) for i in range(hn)]
        self.block = [struct.unpack_from("<IIII", b, i * 16) for i in range(bn)]

    def read(self, name):
        n = len(self.hash)
        a, bh = _hash(name, 1), _hash(name, 2)
        i = start = _hash(name, 0) & (n - 1)
        while True:
            ha, hb, _, _, bi = self.hash[i]
            if bi == 0xFFFFFFFF:
                return None
            if ha == a and hb == bh and bi != 0xFFFFFFFE:
                break
            i = (i + 1) & (n - 1)
            if i == start:
                return None
        pos, csize, fsize, flags = self.block[bi]
        if not flags & 0x80000000:
            return None
        raw = self.d
        pos += self.base
        key = _hash(name.replace("/", "\\").split("\\")[-1], 3)
        if flags & 0x20000:
            key = ((key + (pos - self.base)) ^ fsize) & M32
        enc = bool(flags & 0x10000)
        comp = bool(flags & 0x300)
        if flags & 0x1000000:                      # single unit
            chunks = [(raw[pos:pos + csize], fsize, 0)]
        else:
            ns = (fsize + self.ss - 1) // self.ss
            cnt = ns + 1 + (1 if flags & 0x04000000 else 0)
            offs = raw[pos:pos + cnt * 4]
            if enc:
                offs = _decrypt(offs, (key - 1) & M32)
            offs = struct.unpack("<%dI" % cnt, offs)
            chunks = []
            for s in range(ns):
                exp = min(self.ss, fsize - s * self.ss)
                chunks.append((raw[pos + offs[s]: pos + offs[s + 1]], exp, s))
        out = []
        for data, exp, s in chunks:
            if enc:
                data = _decrypt(data, (key + s) & M32)
            if comp and len(data) < exp:
                data = self._decompress(data, flags, name)
            out.append(data)
        return b"".join(out)[:fsize]

    @staticmethod
    def _decompress(data, flags, name):
        if flags & 0x100:
            return pk_explode(data)
        mask, data = data[0], data[1:]
        if mask == 0x02:
            return zlib.decompress(data)
        if mask == 0x10:
            return bz2.decompress(data)
        if mask == 0x08:
            return pk_explode(data)
        raise NotImplementedError("%s: komprese 0x%02X neni podporovana" % (name, mask))


# ------------------------------------------------------------------ w3i parser
def parse_w3i(d):
    p = 0

    def i32():
        nonlocal p
        v = struct.unpack_from("<i", d, p)[0]
        p += 4
        return v

    def u8():
        nonlocal p
        v = d[p]
        p += 1
        return v

    def s():
        nonlocal p
        e = d.index(b"\0", p)
        v = d[p:e]
        p = e + 1
        return v

    r = {"version": i32()}
    if r["version"] != 25:
        raise ValueError("war3map.w3i verze %d neni podporovana (ocekavana 25)" % r["version"])
    i32(); i32()                      # saves, editor version
    s(); s(); s(); s()                # name, author, description, recommended players
    p += 8 * 4 + 4 * 4                # camera bounds + complements
    r["width"], r["height"], r["flags"] = i32(), i32(), i32()
    u8()                              # tileset
    i32(); s(); s(); s(); s()         # loading screen
    i32(); s(); s(); s(); s()         # prologue
    p += 4 + 4 + 4 + 4 + 4            # fog style, z start, z end, density, color
    p += 4                            # weather
    s(); u8(); p += 4                 # sound env, light env, water tint
    r["players"] = []
    for _ in range(i32()):
        pid, typ, race, _fixed = i32(), i32(), i32(), i32()
        s(); p += 8 + 8               # name, start x/y, ally priorities
        r["players"].append({"id": pid, "type": typ, "race": race})
    r["forces"] = []
    for _ in range(i32()):
        i32(); mask = i32() & M32; s()
        r["forces"].append(mask)
    return r


RACE = {1: 1, 2: 2, 3: 8, 4: 4}      # w3i -> Ghost++ slot race (jinak 32 = random)


def _rol(v, n):
    return ((v << n) | (v >> (32 - n))) & M32


def _xorrol(b):
    v = 0
    n = len(b) // 4 * 4
    for (w,) in struct.iter_unpack("<I", b[:n]):
        v = _rol(v ^ w, 3)
    for c in b[n:]:
        v = _rol(v ^ c, 3)
    return v


MAP_FILES = ["war3map.j", "scripts\\war3map.j", "war3map.w3e", "war3map.wpm", "war3map.doo",
             "war3map.w3u", "war3map.w3b", "war3map.w3d", "war3map.w3a", "war3map.w3q"]


def crc_sha1(mpq, common_j, blizzard_j):
    """Vraci (map_crc, map_sha1) jako 4 resp. 20 bajtu."""
    magic = 0x03F1379E
    val = _xorrol(common_j) ^ _xorrol(blizzard_j)
    val = _rol(val, 3)
    val = _rol(val ^ magic, 3)
    sha = hashlib.sha1(common_j + blizzard_j + struct.pack("<I", magic))
    found = False
    for name in MAP_FILES:
        if found and name.startswith("scripts"):
            continue
        d = mpq.read(name)
        if d is None:
            continue
        if name.endswith("war3map.j"):
            found = True
        sha.update(d)
        val = _rol(val ^ _xorrol(d), 3)
    if not found:
        raise ValueError("v mape chybi war3map.j")
    return struct.pack("<I", val), sha.digest()


def nums(b):
    return " ".join(str(x) for x in b)


def build_cfg(map_file, rel_win, prefix, map_type, jfiles, mm="", mmd=""):
    data = open(map_file, "rb").read()
    warnings = []
    mpq = w3i = None
    try:
        mpq = MPQ(map_file)
        w3i = parse_w3i(mpq.read("war3map.w3i"))
    except Exception as e:
        warnings.append("nelze precist war3map.w3i (%s): chybi map_width, map_height, "
                        "map_numplayers, map_numteams, map_slotN" % e)
    crc = []
    if mpq is not None and jfiles:
        try:
            c, sh = crc_sha1(mpq, *jfiles)
            crc = ["map_crc = " + nums(c), "map_sha1 = " + nums(sh)]
        except Exception as e:
            warnings.append("nelze spocitat map_crc/map_sha1 (%s)" % e)
    elif not jfiles:
        warnings.append("chybi common.j/blizzard.j: chybi map_crc a map_sha1")

    dims = []
    teams = []
    slots = []
    if w3i is not None:
        humans = [pl for pl in w3i["players"] if pl["type"] == 1]
        custom_forces = bool(w3i["flags"] & 0x40)
        for n, pl in enumerate(humans):
            team = n
            if custom_forces:
                team = next((i for i, m in enumerate(w3i["forces"]) if m >> pl["id"] & 1), 0)
            slots.append("map_slot%d = 0 255 0 0 %d %d %d 1 100" % (
                n + 1, team, pl["id"], RACE.get(pl["race"], 32)))
        dims = ["map_width = " + nums(struct.pack("<H", w3i["width"])),
                "map_height = " + nums(struct.pack("<H", w3i["height"]))]
        teams = ["map_numplayers = %d" % len(humans),
                 "map_numteams = %d" % (len(w3i["forces"]) if custom_forces else len(humans))]

    map_path = prefix.rstrip("\\/").replace("/", "\\") + "\\" + rel_win
    banner = "# map file for %s #" % os.path.basename(map_file)
    border = "#" * len(banner)

    L = [
        border, banner, border, "",
        "# the map path tells Warcraft III clients where to find the map on their system",
        "# it is NOT the path to the map on the GHost++ system, in fact GHost++ doesn't need the map at all",
        "",
        "map_path = " + map_path,
        "map_size = " + nums(struct.pack("<I", len(data))),
        "map_info = " + nums(struct.pack("<I", zlib.crc32(data) & M32)),
    ] + crc + [
        "",
        "# map speed", "# 1 = slow", "# 2 = normal", "# 3 = fast", "",
        "map_speed = 3", "",
        "# map visibility", "# 1 = hide terrain", "# 2 = explored", "# 3 = always visible",
        "# 4 = default", "",
        "map_visibility = 4", "",
        "# map observers", "# 1 = none", "# 2 = on defeat", "# 3 = allowed", "# 4 = referees", "",
        "map_observers = 1", "",
        "# map flags",
        "#  - you can combine these flags by adding together all the options you want to use",
        '#  - for example to have teams together and fixed teams you would use "3"',
        "# 1 = teams together", "# 2 = fixed teams", "# 4 = unit share", "# 8 = random hero",
        "# 16 = random races", "",
        "map_flags = 3", "",
        "# map game type", "# 1 = custom", "",
        "map_gametype = 1", "",
        "# map dimensions", "",
    ] + dims + [
        "",
        "# map players and teams", "",
    ] + teams + [
        "",
        "# ",
        "# slot structure",
        "#  [PID] [Download Status] [Slot Status] [Computer] [Team] [Colour] [Race] [Skill] [Handicap]",
        "#  - PID is always 0",
        "#  - Download Status is always 255",
        "#  - Slot Status is 0 for open, 1 for closed, 2 for occupied",
        "#  - Computer is 0 for no, 1 for yes",
        "#  - Team",
        "#  - Colour",
        "#  - Race is 1 for human, 2 for orc, 4 for night elf, 8 for undead, 32 for random (add 64 to any of these for fixed)",
        "#  - Skill is 0 for easy computer, 1 for normal computer or human, 2 for hard computer",
        "#  - Handicap is 50/60/70/80/90/100",
        "# ", "",
    ] + slots + [
        "",
        "# map type (for map specific stats)", "",
        "map_type = " + map_type,
        "map_matchmakingcategory = " + mm,
        "map_statsw3mmdcategory = " + mmd, "",
        "# map local path (for map downloads)",
        "#  GHost++ doesn't require map files but if it has access to them it can send them to players",
        "#  GHost++ will search bot_mappath + map_localpath for the map file (bot_mappath is set in the main config file)",
        "",
        "map_localpath = " + rel_win,
    ]
    return "\r\n".join(L) + "\r\n", warnings


# ------------------------------------------------------------------------ CLI
def find_maps(root, recursive):
    if recursive:
        for dp, _, files in os.walk(root):
            for f in sorted(files):
                if f.lower().endswith(EXTS):
                    yield os.path.join(dp, f)
    else:
        for f in sorted(os.listdir(root)):
            p = os.path.join(root, f)
            if os.path.isfile(p) and f.lower().endswith(EXTS):
                yield p


def write_cfg(map_file, base, outdir, prefix, map_type, overwrite, jfiles, mm="", mmd=""):
    rel = os.path.relpath(map_file, base).replace("/", "\\")
    stem = os.path.splitext(os.path.basename(map_file))[0]
    out_dir = outdir or os.path.dirname(os.path.abspath(map_file))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, stem + ".cfg")

    if os.path.exists(out) and not overwrite:
        print("PRESKOCENO (existuje): " + out)
        return False
    try:
        text, warnings = build_cfg(map_file, rel, prefix, map_type, jfiles, mm, mmd)
    except Exception as e:
        print("CHYBA %s: %s" % (map_file, e))
        return False
    for w in warnings:
        print("VAROVANI %s: %s" % (os.path.basename(map_file), w))
    with open(out, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(out)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="cesta k .w3x/.w3m souboru nebo k adresari s mapami")
    ap.add_argument("-o", "--outdir", help="cilovy adresar pro .cfg (default: vedle mapy)")
    ap.add_argument("-r", "--recursive", action="store_true", help="prochazet i podadresare")
    ap.add_argument("--prefix", default="Maps\\Download",
                    help="prefix pro map_path posilany klientum (default: Maps\\Download)")
    ap.add_argument("--type", default="", help="map_type (napr. dota), default prazdne")
    ap.add_argument("--matchmaking", default="", help="map_matchmakingcategory (napr. dota_elo)")
    ap.add_argument("--mmd", default="", help="map_statsw3mmdcategory")
    ap.add_argument("--jdir", help="adresar s common.j a blizzard.j (default: vedle skriptu)")
    ap.add_argument("--overwrite", action="store_true", help="prepsat existujici .cfg")
    args = ap.parse_args()

    if os.path.isdir(args.input):
        base = args.input
        maps = list(find_maps(base, args.recursive))
        if not maps:
            sys.exit("V adresari nejsou zadne mapy (%s): %s" % (", ".join(EXTS), base))
    elif os.path.isfile(args.input):
        base = os.path.dirname(os.path.abspath(args.input))
        maps = [args.input]
    else:
        sys.exit("Cesta neexistuje: " + args.input)

    jdir = args.jdir or os.path.dirname(os.path.abspath(__file__))
    jp = [os.path.join(jdir, n) for n in ("common.j", "blizzard.j")]
    jfiles = None
    if all(os.path.isfile(x) for x in jp):
        jfiles = tuple(open(x, "rb").read() for x in jp)

    done = sum(write_cfg(m, base, args.outdir, args.prefix, args.type, args.overwrite, jfiles, args.matchmaking, args.mmd)
               for m in maps)
    print("Hotovo: %d z %d cfg vytvoreno." % (done, len(maps)))


if __name__ == "__main__":
    main()
