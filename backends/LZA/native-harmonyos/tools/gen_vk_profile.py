#!/usr/bin/env python3
"""Generate overlay/android/gfx/vk_profile_gen.inc from the Vulkan registry (vk.xml): the printers and tables the
runtime's Vulkan Profiles export (overlay/android/gfx/vk_profile.inc, TOTK_VK_PROFILE) uses, so the phone's driver can be
written out in the format `vulkaninfo --json` writes (profiles-0.8 schema) although HarmonyOS ships no vulkaninfo and
does not run self-built programs from hdc shell.

  python native-harmonyos/tools/gen_vk_profile.py <vk.xml> [out.inc]

vk.xml: registry/vk.xml of a Vulkan-Headers checkout, required (any registry at least as new as the headers
the runtime builds with). Every struct is compiled only when the headers define it (#if defined(VK_VERSION_x_y) /
defined(<extension>)), with the type name those headers give it (an extension's alias name works in old and new headers
alike), and its sType, enum and flag values are numbers from the registry, so one generated file builds against the
HarmonyOS SDK's headers (309), the container's (275) and newer ones.

What vulkaninfo's profile holds, and so what is generated:
- features: VkPhysicalDeviceFeatures and every struct extending VkPhysicalDeviceFeatures2;
- properties: VkPhysicalDeviceProperties and every struct extending VkPhysicalDeviceProperties2;
- formats: VkFormatProperties, VkFormatProperties3 and VkSubpassResolvePerformanceQueryEXT for every format;
- queue families: VkQueueFamilyProperties and VkQueueFamilyGlobalPriorityProperties.
A struct promoted to a core version the device has is covered by that version's VkPhysicalDeviceVulkanXYFeatures /
Properties and is left out, as vulkaninfo does. Structs that cannot be written generically (handles, function pointers,
pointers to structs, unions) are left out; the generator lists them.
"""
import re
import sys
import xml.etree.ElementTree as ET

if len(sys.argv) < 2:
    sys.exit("usage: python native-harmonyos/tools/gen_vk_profile.py <vk.xml> [out.inc]   (Vulkan-Headers' registry/vk.xml)")
VKXML = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else None

root = ET.parse(VKXML).getroot()


def for_vulkan(e):
    api = e.get("api")
    return api is None or "vulkan" in api.split(",")


# ---- types ----------------------------------------------------------------------------------------------------------
types = {}      # name -> element (vulkan API only)
alias_of = {}   # alias type name -> canonical name
category = {}   # type name -> category
for t in root.find("types"):
    if t.tag != "type" or not for_vulkan(t):
        continue
    name = t.get("name") or (t.find("name").text if t.find("name") is not None else None)
    if not name:
        continue
    category[name] = t.get("category")
    if t.get("alias"):
        alias_of[name] = t.get("alias")
        continue
    types[name] = t


def canon(n):
    while n in alias_of:
        n = alias_of[n]
    return n


bits_of = {}    # bitmask type -> its FlagBits enum (32 or 64 bit)
flag64 = set()
for name, t in types.items():
    if category.get(name) == "bitmask":
        b = t.get("requires") or t.get("bitvalues")
        if b:
            bits_of[name] = b
        ty = t.find("type")
        if ty is not None and ty.text == "VkFlags64":
            flag64.add(name)

# ---- enums ----------------------------------------------------------------------------------------------------------
enum_vals = {}  # enum type -> {name: value}
enum_alias = {}  # enumerant alias -> canonical
for es in root.findall("enums"):
    if not for_vulkan(es):
        continue
    ename = es.get("name")
    d = enum_vals.setdefault(ename, {})
    for e in es.findall("enum"):
        if not for_vulkan(e):
            continue
        if e.get("alias"):
            enum_alias[e.get("name")] = e.get("alias")
        elif e.get("bitpos") is not None:
            d[e.get("name")] = 1 << int(e.get("bitpos"))
        elif e.get("value") is not None:
            v = e.get("value").rstrip("ULLF")
            try:
                d[e.get("name")] = int(v, 0)
            except ValueError:
                pass


def ext_enum_value(e, extnumber):
    if e.get("bitpos") is not None:
        return 1 << int(e.get("bitpos"))
    if e.get("value") is not None:
        v = e.get("value").strip('"').rstrip("ULLF")
        try:
            return int(v, 0)
        except ValueError:
            return None
    if e.get("offset") is not None:
        n = int(e.get("extnumber") or extnumber)
        v = 1000000000 + (n - 1) * 1000 + int(e.get("offset"))
        return -v if e.get("dir") == "-" else v
    return None


def version_tuple(s):
    a, b = s.split(".")
    return int(a), int(b)


# requirements: what makes a type / enumerant available
type_core = {}    # canonical type -> lowest core version (major, minor)
type_exts = {}    # canonical type -> [(extension, name used there)]
enum_core = {}    # enumerant -> core version
enum_exts = {}    # enumerant -> [extension]
ext_ok = {}       # extension name -> usable here (vulkan API, no platform header, not disabled)
for f in root.findall("feature"):
    if not for_vulkan(f):
        continue
    ver = version_tuple(f.get("number"))
    for req in f.findall("require"):
        if not for_vulkan(req):
            continue
        for t in req.findall("type"):
            c = canon(t.get("name"))
            if c not in type_core or ver < type_core[c]:
                type_core[c] = ver
        for e in req.findall("enum"):
            if not for_vulkan(e):
                continue
            n = e.get("name")
            if e.get("extends"):
                if e.get("alias"):
                    enum_alias[n] = e.get("alias")
                else:
                    v = ext_enum_value(e, 0)
                    if v is not None:
                        enum_vals.setdefault(e.get("extends"), {})[n] = v
            if n not in enum_core or ver < enum_core[n]:
                enum_core[n] = ver

for x in root.find("extensions"):
    name = x.get("name")
    sup = (x.get("supported") or "").split(",")
    usable = "vulkan" in sup and not x.get("platform") and x.get("provisional") != "true"
    ext_ok[name] = usable
    if "vulkan" not in sup:
        continue
    num = int(x.get("number"))
    for req in x.findall("require"):
        if not for_vulkan(req):
            continue
        for e in req.findall("enum"):
            if not for_vulkan(e) or not e.get("extends"):
                continue
            n = e.get("name")
            if e.get("alias"):
                enum_alias[n] = e.get("alias")
            else:
                v = ext_enum_value(e, num)
                if v is not None:
                    enum_vals.setdefault(e.get("extends"), {})[n] = v
            if usable:
                enum_exts.setdefault(n, []).append(name)
        if not usable:
            continue
        for t in req.findall("type"):
            c = canon(t.get("name"))
            lst = type_exts.setdefault(c, [])
            if (name, t.get("name")) not in lst:
                lst.append((name, t.get("name")))


def canon_enum(n):
    while n in enum_alias:
        n = enum_alias[n]
    return n


# an extension's enumerants listed under their alias names count for the canonical ones
for n in list(enum_exts):
    c = canon_enum(n)
    if c != n:
        for x in enum_exts[n]:
            if x not in enum_exts.setdefault(c, []):
                enum_exts[c].append(x)

stype = enum_vals["VkStructureType"]

# ---- struct members -------------------------------------------------------------------------------------------------


class Member:
    def __init__(self, m):
        self.type = m.find("type").text
        self.name = m.find("name").text
        text = "".join(m.itertext())
        after = text.split(self.name, 1)[1] if self.name in text else ""
        before = text.split(self.name, 1)[0]
        self.ptr = "*" in before
        self.array = "[" in after
        self.len = m.get("len")
        self.values = m.get("values")


def members(sname):
    t = types[sname]
    return [Member(m) for m in t.findall("member") if for_vulkan(m)]


NUM = {"uint8_t": "u", "uint16_t": "u", "uint32_t": "u", "uint64_t": "u", "size_t": "u", "VkDeviceSize": "u",
       "VkDeviceAddress": "u", "int8_t": "i", "int16_t": "i", "int32_t": "i", "int64_t": "i", "float": "f",
       "double": "f", "VkBool32": "b", "char": "s", "VkFlags": "u", "VkFlags64": "u"}

used_enums = set()
used_flags = set()
used_nested = []       # nested struct types needing printers, in dependency order
skipped = {}


def kind(m):
    """How a member is written: None when the struct cannot be written generically."""
    c = category.get(m.type)
    if m.type in NUM:
        return NUM[m.type]
    if c == "enum":
        used_enums.add(m.type)
        return "e"
    if c == "bitmask":
        used_flags.add(m.type)
        return "F"
    if c == "struct" and not m.ptr:
        nested = canon(m.type)
        if nested in types and not any(x.values for x in members(nested)):
            if writable(nested) and nested not in used_nested:
                used_nested.append(nested)
            return "n" if writable(nested) else None
    return None


_writable = {}


def writable(sname):
    if sname in _writable:
        return _writable[sname]
    _writable[sname] = False
    t = types.get(sname)
    if t is None or t.get("category") != "struct":
        return False
    ok = True
    ms = members(sname)
    names = {m.name for m in ms}
    for m in ms:
        if m.name in ("sType", "pNext"):
            continue
        if m.ptr:
            # an output array the driver fills (count first): numbers or enums only
            if not m.len or m.len not in names or m.array:
                ok = False
                break
            c = category.get(m.type)
            if not (m.type in NUM and m.type != "char") and c != "enum":
                ok = False
                break
            if c == "enum":
                used_enums.add(m.type)
            continue
        if kind(m) is None:
            ok = False
            break
    _writable[sname] = ok
    return ok


def extends(sname, base):
    return base in (types[sname].get("structextends") or "").split(",")


def c_ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def guard_options(sname):
    """[(macro condition, C type name)] in preference order; [] = not available in any header we can name."""
    opts = []
    if sname in type_core:
        maj, mi = type_core[sname]
        opts.append((f"defined(VK_VERSION_{maj}_{mi})", sname))
    for ext, used in type_exts.get(sname, []):
        if ext_ok.get(ext):
            opts.append((f"defined({ext})", used))
    return opts


def emit_typedef(out, ident, opts):
    first = True
    for cond, cname in opts:
        out.append(f"#{'if' if first else 'elif'} {cond}\ntypedef {cname} pf_t_{ident};")
        first = False
    out.append("#endif")
    cond_any = " || ".join(c for c, _ in opts)
    return cond_any


def emit_printer(out, ident, sname):
    ms = members(sname)
    out.append(f"static void pf_print_{ident}(pj *j, const void *v) {{\n    const pf_t_{ident} *s = (const pf_t_{ident} *)v; (void)s;")
    for m in ms:
        if m.name in ("sType", "pNext"):
            continue
        n = m.name
        if m.ptr:
            if category.get(m.type) == "enum":
                out.append(f'    pj_key_arr(j, "{n}"); for (uint32_t i = 0; s->{n} && i < s->{m.len}; i++) pj_enum(j, pf_names_{c_ident(m.type)}, (int64_t)s->{n}[i]); pj_end_arr(j);')
            else:
                k = NUM[m.type]
                out.append(f'    pj_key_arr(j, "{n}"); for (uint32_t i = 0; s->{n} && i < s->{m.len}; i++) pj_num_{k}(j, s->{n}[i]); pj_end_arr(j);')
            continue
        k = kind(m)
        if m.name == "deviceLUID":
            out.append(f'    if (s->deviceLUIDValid) {{')
        if m.array and k == "s":
            out.append(f'    pj_key(j, "{n}"); pj_str_n(j, s->{n}, sizeof s->{n});')
        elif m.array:
            # a fixed array with a count member (priorities[VK_MAX_GLOBAL_PRIORITY_SIZE], len="priorityCount"): only
            # the entries the count covers, as vulkaninfo writes them
            bound = f"sizeof s->{n} / sizeof s->{n}[0]"
            if m.len and m.len in {x.name for x in ms}:
                bound = f"(s->{m.len} < {bound} ? s->{m.len} : {bound})"
            if k in "uifb":
                out.append(f'    pj_key_arr(j, "{n}"); for (size_t i = 0; i < {bound}; i++) pj_num_{k}(j, s->{n}[i]); pj_end_arr(j);')
            elif k == "e":
                used_enums.add(m.type)
                out.append(f'    pj_key_arr(j, "{n}"); for (size_t i = 0; i < {bound}; i++) pj_enum(j, pf_names_{c_ident(m.type)}, (int64_t)s->{n}[i]); pj_end_arr(j);')
            elif k == "n":
                out.append(f'    pj_key_arr(j, "{n}"); for (size_t i = 0; i < sizeof s->{n} / sizeof s->{n}[0]; i++) {{ pj_obj(j); pf_print_{c_ident(canon(m.type))}(j, &s->{n}[i]); pj_end_obj(j); }} pj_end_arr(j);')
            else:
                out.append(f'    /* {n}: array of {m.type} not written */')
        elif k in "uifb":
            out.append(f'    pj_key(j, "{n}"); pj_num_{k}(j, s->{n});')
        elif k == "e":
            out.append(f'    pj_key(j, "{n}"); pj_enum(j, pf_names_{c_ident(m.type)}, (int64_t)s->{n});')
        elif k == "F":
            bits = bits_of.get(m.type)
            table = f"pf_names_{c_ident(bits)}" if bits else "pf_names_none"
            out.append(f'    pj_key(j, "{n}"); pj_flags(j, {table}, (uint64_t)s->{n});')
        elif k == "n":
            out.append(f'    pj_key(j, "{n}"); pj_obj(j); pf_print_{c_ident(canon(m.type))}(j, &s->{n}); pj_end_obj(j);')
        if m.name == "deviceLUID":
            out.append("    }")
    out.append("}")


def has_pointer_arrays(sname):
    return any(m.ptr and m.name != "pNext" for m in members(sname))


def emit_fill(out, ident, sname):
    """Second query of a struct with output arrays: allocate them from the counts the first query wrote."""
    ms = members(sname)
    out.append(f"static int pf_fill_{ident}(void *v) {{\n    pf_t_{ident} *s = (pf_t_{ident} *)v; int any = 0;")
    for m in ms:
        if m.ptr and m.name != "pNext":
            out.append(f"    if (s->{m.len}) {{ s->{m.name} = calloc(s->{m.len}, sizeof *s->{m.name}); any = 1; }}")
    out.append("    return any;\n}")
    out.append(f"static void pf_free_{ident}(void *v) {{\n    pf_t_{ident} *s = (pf_t_{ident} *)v;")
    for m in ms:
        if m.ptr and m.name != "pNext":
            out.append(f"    free((void *)s->{m.name});")
    out.append("}")


# ---- choose the structs -----------------------------------------------------------------------------------------------
def gather(base, only=None):
    res = []
    for sname, t in types.items():
        if t.get("category") != "struct" or not extends(sname, base):
            continue
        if only and sname not in only:
            continue
        if not writable(sname):
            skipped[sname] = base
            continue
        st = next((m.values for m in members(sname) if m.name == "sType"), None)
        if not st or st not in stype:
            skipped[sname] = base + " (no sType value)"
            continue
        opts = guard_options(sname)
        if not opts:
            continue
        res.append((sname, stype[st], opts))
    return sorted(res)


FEAT = gather("VkPhysicalDeviceFeatures2")
PROP = gather("VkPhysicalDeviceProperties2")
FMT = gather("VkFormatProperties2", {"VkFormatProperties3", "VkSubpassResolvePerformanceQueryEXT"})
QF = gather("VkQueueFamilyProperties2", {"VkQueueFamilyGlobalPriorityProperties"})
AGG = {"VkPhysicalDeviceVulkan11Features", "VkPhysicalDeviceVulkan12Features", "VkPhysicalDeviceVulkan13Features",
       "VkPhysicalDeviceVulkan14Features", "VkPhysicalDeviceVulkan11Properties", "VkPhysicalDeviceVulkan12Properties",
       "VkPhysicalDeviceVulkan13Properties", "VkPhysicalDeviceVulkan14Properties"}
BASE = ["VkPhysicalDeviceFeatures", "VkPhysicalDeviceProperties", "VkFormatProperties", "VkQueueFamilyProperties"]
for b in BASE:
    assert writable(b), b

# ---- emit ------------------------------------------------------------------------------------------------------------
hv = next((t for t in root.find("types") if t.tag == "type" and (t.findtext("name") == "VK_HEADER_VERSION")), None)
hv = "".join(hv.itertext()).split()[-1] if hv is not None else "?"
out = [f"/* Generated by native-harmonyos/tools/gen_vk_profile.py from vk.xml (VK_HEADER_VERSION {hv}); do not edit.\n"
       "   Included by vk_profile.inc. */"]


def emit_names(tname, vals, bits):
    ident = c_ident(tname)
    seen = set()
    rows = []
    for n, v in vals.items():
        if v in seen:
            continue
        if bits and (v == 0 or v & (v - 1)):
            continue
        seen.add(v)
        val = f"{v}ULL" if v >= 0 else f"(uint64_t)({v}LL)"
        rows.append(f'    {{{val}, "{n}"}},')
    out.append(f"static const pf_name pf_names_{ident}[] = {{\n" + "\n".join(rows) + "\n    {0, NULL}\n};")


# nested printers first (dependency order), then the extension structs
used_enums.update({"VkFormat", "VkPhysicalDeviceType", "VkStructureType"})
used_flags_bits = {bits_of[f] for f in used_flags if f in bits_of} | {"VkFormatFeatureFlagBits", "VkQueueFlagBits"}
if "VkFormatFeatureFlagBits2" in enum_vals:
    used_flags_bits.add("VkFormatFeatureFlagBits2")
out.append("static const pf_name pf_names_none[] = {{0, NULL}};")
for e in sorted(used_enums - used_flags_bits):   # a FlagBits type used as a single value shares the bit table
    emit_names(e, enum_vals.get(e, {}), False)
for b in sorted(used_flags_bits):
    emit_names(b, enum_vals.get(b, {}), True)

body = []
emitted = set()


def emit_struct(sname):
    if sname in emitted:
        return None
    emitted.add(sname)
    ident = c_ident(sname)
    opts = guard_options(sname) or [("1", sname)]
    cond = emit_typedef(body, ident, opts)
    body.append(f"#if {cond}\n#define PF_HAVE_{ident} 1")
    emit_printer(body, ident, sname)
    if has_pointer_arrays(sname):
        emit_fill(body, ident, sname)
    body.append("#endif")
    return cond


# nested structs (used_nested lists the deepest first: writable() recurses before appending)
for n in used_nested:
    emit_struct(n)
for b in BASE:
    emit_struct(b)


def table(label, entries):
    rows = []
    for sname, st, opts in entries:
        emit_struct(sname)
        ident = c_ident(sname)
        core = type_core.get(sname)
        corev = f"VK_MAKE_API_VERSION(0, {core[0]}, {core[1]}, 0)" if core else "0"
        # when the device has several extensions giving the struct, the KHR name first, then EXT, then a vendor's
        rank = lambda eu: (0 if eu[0].startswith("VK_KHR_") else 1 if eu[0].startswith("VK_EXT_") else 2)
        exts = "|".join(f"{e}={used}" for e, used in sorted(type_exts.get(sname, []), key=rank) if ext_ok.get(e))
        covered = "0"
        if sname not in AGG and core and core >= (1, 1) and label in ("feat", "prop") and core <= (1, 4):
            covered = f"PF_AGG_{label}_{core[0]}_{core[1]}"
        fill = f"pf_fill_{ident}, pf_free_{ident}" if has_pointer_arrays(sname) else "NULL, NULL"
        rows.append(f"#ifdef PF_HAVE_{ident}\n    {{{st}, sizeof(pf_t_{ident}), {corev}, \"{sname}\", \"{exts}\", {covered}, pf_print_{ident}, {fill}}},\n#endif")
    return rows


def agg_fn(label, kindname):
    """PF_AGG_<label>_<maj>_<min>: 1 when the headers have that version's VkPhysicalDeviceVulkanXY<kind> (it then
    carries every struct promoted to that version)."""
    lines = []
    for v in ((1, 1), (1, 2), (1, 3), (1, 4)):
        ident = c_ident(f"VkPhysicalDeviceVulkan{v[0]}{v[1]}{kindname}")
        lines.append(f"#ifdef PF_HAVE_{ident}\n#define PF_AGG_{label}_{v[0]}_{v[1]} 1\n#else\n#define PF_AGG_{label}_{v[0]}_{v[1]} 0\n#endif")
    return lines


feat_rows = table("feat", FEAT)
prop_rows = table("prop", PROP)
fmt_rows = table("fmt", FMT)
qf_rows = table("qf", QF)
out += body
out += agg_fn("feat", "Features") + agg_fn("prop", "Properties")
# the aggregates' own presence is known only after their printers: re-emit the tables after the functions
for label, rows in (("feat", feat_rows), ("prop", prop_rows), ("fmt", fmt_rows), ("qf", qf_rows)):
    out.append(f"static const pf_struct pf_{label}[] = {{")
    out += rows
    out.append("    {0, 0, 0, NULL, NULL, 0, NULL, NULL, NULL}\n};")

# formats: every VkFormat with what makes it available
fmt = []
for n, v in sorted(enum_vals["VkFormat"].items(), key=lambda kv: (kv[1] < 0, abs(kv[1]))):
    if v == 0:
        continue
    core = enum_core.get(n, (1, 0) if 0 < v < 1000000000 else None)
    exts = "|".join(enum_exts.get(n, []))
    if not core and not exts:
        continue
    corev = f"VK_MAKE_API_VERSION(0, {core[0]}, {core[1]}, 0)" if core else "0"
    fmt.append(f'    {{{v}, "{n}", {corev}, "{exts}"}},')
out.append("static const pf_format pf_formats[] = {\n" + "\n".join(fmt) + "\n    {0, NULL, 0, NULL}\n};")

text = "\n".join(out) + "\n"
if OUT:
    open(OUT, "w", newline="\n").write(text)
else:
    sys.stdout.write(text)
sys.stderr.write(f"features {len(FEAT)}, properties {len(PROP)}, format structs {len(FMT)}, queue family structs {len(QF)}, "
                 f"formats {len(fmt)}, enums {len(used_enums)}, flag types {len(used_flags_bits)}; not writable: "
                 + ", ".join(sorted(skipped)) + "\n")
