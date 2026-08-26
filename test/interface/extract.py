#!/usr/bin/env python3
"""Print the built mod's observable interface, so a refactor can be checked against it.

Everything here is read out of the compiled library, never out of the source, so it
describes what actually ships. Output is sorted and deterministic: diff two runs.

usage: extract.py <path to libnickeltypefix.so>
"""
import collections, hashlib, re, struct, sys

blob = open(sys.argv[1], "rb").read()

e_shoff, = struct.unpack_from("<I", blob, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x2e)
FIELDS = "name typ flags addr off size link info align entsize".split()
secs = [dict(zip(FIELDS, struct.unpack_from("<10I", blob, e_shoff + i * e_shentsize)))
        for i in range(e_shnum)]
shstr = secs[e_shstrndx]

def sname(s):
    b = blob[shstr["off"] + s["name"]:]
    return b[:b.index(b"\0")].decode()

by = {sname(s): s for s in secs}

def vaddr_to_off(a):
    for s in secs:
        if s["addr"] and s["addr"] <= a < s["addr"] + s["size"] and s["typ"] != 8:
            return s["off"] + (a - s["addr"])
    return None

def cstr(a, limit=512):
    o = vaddr_to_off(a)
    if o is None:
        return None
    end = blob.find(b"\0", o, o + limit)
    if end < 0:
        return None
    try:
        return blob[o:end].decode("utf-8")
    except UnicodeDecodeError:
        return None

out = []

# 1. The hook bodies the mod exports. NickelHook redirects a target to these by name, so
#    this list is the mod's contract with the loader: renaming or dropping one changes it.
ds, dstr = by[".dynsym"], by[".dynstr"]
exported = set()
for o in range(ds["off"], ds["off"] + ds["size"], 16):
    nm, val, sz, info, other, shndx = struct.unpack_from("<IIIBBH", blob, o)
    if shndx == 0:
        continue
    b = blob[dstr["off"] + nm:]
    n = b[:b.index(b"\0")].decode()
    if n.startswith("_ntf_"):
        exported.add(n)
out.append("[exported hook bodies]")
out += sorted(exported)

# 2. Everything the mod names in the host process: the symbols it hooks or resolves, and
#    the libraries it looks them up in.
ro = by.get(".rodata")
text = blob[ro["off"]:ro["off"] + ro["size"]] if ro else b""

# The build stamps its git version into several strings. Left alone, the snapshot would
# differ on every commit and say nothing about the code, so blank the version out first.
# Normalise before measuring length: the stamp is not a fixed width, and a literal near the
# threshold below would otherwise drop in or out of the digest as the version grew.
VERSION = re.compile(r"v\d+\.\d+(?:\.\d+)?(?:-\d+-g[0-9a-f]{6,})?(?:-dirty)?|\bdev\b")
strs = [VERSION.sub("<version>", s.decode("latin-1"))
        for s in re.findall(rb"[\x20-\x7e]{4,}", text)]
out.append("")
out.append("[hooked and resolved symbols]")
out += sorted({s for s in strs if re.fullmatch(r"_Z[A-Za-z0-9_]{4,}", s)})
out.append("")
out.append("[target libraries]")
out += sorted({s for s in strs if re.fullmatch(r"lib[A-Za-z0-9]+\.so[0-9.]*", s)})

# 3. Config keys with their shipped defaults, read from the table in .data.rel.ro rather
#    than from the source: a record is three consecutive pointers whose first is an
#    "ntf_" string. This catches a default being flipped, which a string dump would not.
out.append("")
out.append("[config keys and defaults]")
keys = []
for secname in (".data.rel.ro", ".data.rel.ro.local", ".data", ".rodata"):
    s = by.get(secname)
    if not s:
        continue
    for off in range(s["off"], s["off"] + s["size"] - 12, 4):
        p1, p2, p3 = struct.unpack_from("<III", blob, off)
        k = cstr(p1, 64) if p1 else None
        if not k or not re.fullmatch(r"ntf_[a-z_]+", k):
            continue
        d = cstr(p2, 64) if p2 else ""
        desc = cstr(p3, 400) if p3 else ""
        if d is None or desc is None:
            continue
        keys.append((k, d))
out += [f"{k} = {d!r}" for k, d in sorted(set(keys))]

# 4. The config file the mod writes on a fresh install. This is a separate source of truth
#    from the table above, and the two silently disagreeing is a bug that ships: the table
#    only fills in keys an existing file is missing, so a wrong value here reaches every new
#    install and nothing else notices. Read from the raw bytes rather than the string list,
#    because the template spans many lines and the list is split on newlines.
out.append("")
out.append("[fresh-install config file]")
written_pairs = []
# Anchor on the file's own header: "ntf_enabled:" also appears in a log message.
idx = text.find(b"# NickelTypeFix configuration")
if idx >= 0:
    lo = text.rfind(b"\0", 0, idx) + 1
    hi = text.find(b"\0", idx)
    tpl = text[lo:hi].decode("utf-8", "replace")
    written_pairs = re.findall(r"^(ntf_[a-z_]+):(.*)$", tpl, re.M)
written = dict(written_pairs)
for k in sorted(written):
    out.append(f"{k} = {written[k]!r}")

out.append("")
out.append("[table vs file disagreements]")
table = dict(keys)
table_counts = collections.Counter(k for k, _ in keys)
written_counts = collections.Counter(k for k, _ in written_pairs)
bad = [f"{k}: duplicate table key ({n} entries)"
       for k, n in sorted(table_counts.items()) if n > 1]
bad += [f"{k}: duplicate file key ({n} entries)"
        for k, n in sorted(written_counts.items()) if n > 1]
bad += [f"{k}: missing from file" for k in sorted(table.keys() - written.keys())]
bad += [f"{k}: missing from table" for k in sorted(written.keys() - table.keys())]
bad += [f"{k}: table {table[k]!r}, file {written[k]!r}"
        for k in sorted(table.keys() & written.keys())
        if written[k] != table[k]]
out += bad or ["none"]

# 5. A digest of the long string literals. The injected CSS and the scripts the mod runs
#    in the book's frame live here, so a change in behaviour that leaves the hook list
#    untouched still shows up as a different digest.
longs = sorted({s for s in strs if len(s) >= 40})
out.append("")
out.append("[long string literals]")
out.append(f"count  {len(longs)}")
out.append(f"sha256 {hashlib.sha256(chr(10).join(longs).encode()).hexdigest()}")

print("\n".join(out))
