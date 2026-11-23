#!/usr/bin/env python3

from functools import reduce

class ABI:
    def __init__(self, spec):
        self.spec = spec

    def each(self, sect):
        for n, spec in sorted(self.spec[sect].items()):
            yield { "name": n } | (spec if spec else {})


class Design:
    class Field:
        def __init__(self, reg, cfg):
            self.reg, self.cfg = reg, cfg

        def sliced(self, abi):
            return self.slice(*abi["slice"]) if "slice" in abi else self

        def slice(self, lo, hi):
            start, end = self.cfg["Start bit"], self.cfg["End bit"]
            if hi < lo or start + hi > end:
                raise ValueError(f"Invalid slice [{lo}:{hi}] of field \"{self.cfg['Name']}\"")

            return Design.Field(self.reg, self.cfg | {
                "Start bit": start + lo,
                "End bit": start + hi,
            })

        def initializer(self, var):
            start, end = self.cfg["Start bit"], self.cfg["End bit"]

            offs = start // 32
            mask = (1 << (end + 1)) - 1
            mask >>= start
            mask <<= start
            mask >>= offs * 32
            mask_hi = mask >> 32
            mask &= (1 << 32) - 1

            if mask_hi:
                out = f"{{ .offs = {offs}, .mask = 0x{mask:08x}, .mask_hi = 0x{mask_hi:08x} }}"
            else:
                out = f"{{ .offs = {offs}, .mask = 0x{mask:08x} }}"

            if var:
                out = f"{{ .reg = {self.reg.cfg['Start address']:>5}, .field = {out} }}"

            return out

    class Stat:
        def __init__(self, long, cfg):
            self.long, self.cfg = long, cfg
            self.short = long.lower().replace(" ", "_").replace(":", "")
            self.nr_fields = 0

        def fields(self, name):
            out = f"static const struct pasw_stat_field saline_stat_{self.short}_fields[] = {{\n"
            for fcfg in self.cfg["Fields"]:
                f = Design.Field(self, fcfg)
                out += f"\t{{ .name = \"{fcfg['Name']}\", .field = {f.initializer(False)} }},\n"
                self.nr_fields += 1
            out += "};\n"
            return out

        def table(self):
            out = "\t{\n"
            out += "\t\t.table = { "
            out += f".reg = {self.cfg['Start address']}, "
            out += f".len = {self.cfg['Number of entries']}, "
            out += f".stride = {self.cfg['Address step']}, "
            out += "},\n"
            out += f"\t\t.name = \"{self.short}\",\n"
            out += f"\t\t.nr_fields = {self.nr_fields},\n"
            out += f"\t\t.field = saline_stat_{self.short}_fields,\n"
            out += "\t},"

            return out

    class Reg:
        def __init__(self, long, cfg):
            self.long, self.cfg = long, cfg

        def field(self, name):
            segs = name.split("_")
            camel = "".join([segs[0]] + [s.capitalize() for s in segs[1:]])
            for fcfg in self.cfg["Fields"]:
                if fcfg["Name"] in (name, camel):
                    return Design.Field(self, fcfg)

            return None

    class Regs:
        def __init__(self, spec):
            self.spec = spec

        def each_stat(self):
            for long, spec in sorted(self.spec["conf bus 0"].items()):
                if not spec.get("Statistics"):
                    continue
                yield Design.Stat(long, spec)

        def reg_by_name(self, name):
            if spec := self.spec["conf bus 0"].get(name):
                return Design.Reg(name, spec)

            return None

        def reg_by_abi(self, abi):
            name = abi.get("src", abi["name"])
            return self.reg_by_name(name)

        def field(self, name, field):
            r = self.reg_by_name(name)
            if not r:
                return None

            return r.field(field)

    class Conf:
        def __init__(self, spec):
            self.spec = spec
            self._computed_attrs = {
                "cpu_port_format": self._cpu_port_format,
            }

        def _cpu_port_format(self, abi):
            if self.spec["defines"].get("c_CPU_TAG_QUEUE_BYTE"):
                return "PASW_TAG_2B_PORT_QUEUE"
            return "PASW_TAG_UNSUPPORTED"

        def attr_by_abi(self, abi):
            if abi["name"] in self._computed_attrs:
                return self._computed_attrs[abi["name"]](abi)

            src = abi.get("src", abi["name"])
            match src:
                case str():
                    path = [src]
                case list():
                    path = src

            attr = reduce(lambda spec, key: spec.get(key, {}), path, self.spec["hwconf"])
            if not attr:
                return None

            if abi.get("type") == "const char *":
                return "\"" + attr + "\""

            return attr


    def __init__(self, name, abi, regs, conf):
        self.name, self.abi, self.regs, self.conf = name, abi, Design.Regs(regs), Design.Conf(conf)
        self.tables = []

    def available(self, abi, reg=None, field=None, table=None):
        if reg or field:
            return True

        if not abi.get("mandatory", True):
            return False

        errmsg = f"Design \"{self.name}\" does not implement mandatory "
        if field:
            if table:
                errmsg += f"field \"{abi['name']}\" in table \"{table}\""
            else:
                errmsg += f"variable \"{abi['name']}\""
        else:
                errmsg += f"table \"{abi['name']}\""

        raise ValueError(errmsg)

    def emit(self, pr):
        nr_stats = self.emit_stats(pr)
        self.emit_tables(pr)

        pr.writeln(f"static const struct pasw_abi {self.name}_pasw_abi = {{")
        pr.writeln("\t.attr = {")
        self.emit_attrs(pr)
        pr.writeln("\t},")

        pr.writeln("\t.var = {")
        align = alignment(self.abi.spec["variables"])
        for vabi in self.abi.each("variables"):
            f = self.regs.field(*vabi["src"])
            if not self.available(vabi, field=f):
                continue

            f = f.sliced(vabi)
            pr.writeln(f"\t\t.{vabi['name']:<{align}} = {f.initializer(True)},")
        pr.writeln("\t},")

        if self.tables:
            pr.writeln("\t.table = {")
            for t in self.tables:
                pr.writeln(f"\t\t.{t} = &{self.name}_pasw_t_{t},")
            pr.writeln("\t},")

        if nr_stats:
            pr.writeln("\t.stats = {")
            pr.writeln(f"\t\t.nr_stats = {nr_stats},")
            pr.writeln(f"\t\t.table = {self.name}_pasw_stat_tables,")
            pr.writeln("\t},")

        pr.writeln("};")

    def emit_stats(self, pr):
        tables = ""
        nr_stats = 0
        for stat in self.regs.each_stat():
            pr.writeln(stat.fields(self.name))
            if tables:
                tables += "\n"
            tables += stat.table()
            nr_stats += 1

        pr.writeln(f"static const struct pasw_stat_table {self.name}_pasw_stat_tables[] = {{")
        pr.writeln(tables)
        pr.writeln("};\n")
        return nr_stats

    def emit_tables(self, pr):
        for tabi in self.abi.each("tables"):
            self.emit_table(pr, tabi)

    def emit_table(self, pr, tabi):
        name = tabi["name"]

        reg = self.regs.reg_by_abi(tabi)
        if not self.available(tabi, reg=reg):
            return

        replica = self.regs.reg_by_name(tabi["replica"]) if "replica" in tabi else None

        self.tables.append(name)

        pr.writeln(f"static const struct pasw_t_{name} {self.name}_pasw_t_{name} = {{")
        pr.writeln("\t.table = {")
        pr.writeln(f"\t\t.reg    = {reg.cfg['Start address']:>5},")
        pr.writeln(f"\t\t.stride = {reg.cfg['Address step']:>5},")
        pr.writeln(f"\t\t.len    = {reg.cfg['Number of entries']:>5},")
        if ways := reg.cfg.get("Number of tables"):
            pr.writeln(f"\t\t.ways   = {ways:>5},")
        if replica:
            pr.writeln(f"\n\t\t.replica_reg = {replica.cfg['Start address']},")
        pr.writeln("\t},")

        align = alignment(tabi["fields"])
        for f, fabi in sorted(tabi["fields"].items()):
            fabi = fabi if fabi else {}
            rf = reg.field(fabi.get("src", f))
            if not self.available(fabi, field=rf, table=tabi["name"]):
                continue

            rf = rf.sliced(fabi)
            pr.writeln(f"\t.{f:<{align}} = {rf.initializer(False)},")

        pr.writeln("};\n")

    def emit_attrs(self, pr):
        align = alignment(self.abi.spec["attributes"])
        for aabi in self.abi.each("attributes"):
            self.emit_attr(pr, aabi, align)

    def emit_attr(self, pr, aabi, align):
        a = self.conf.attr_by_abi(aabi)
        if a == None:
            if aabi.get("mandatory", True):
                raise ValueError(f"Design \"{self.name}\" does not define mandatory attribute \"{aabi['name']}\"")
            return

        pr.writeln(f"\t\t.{aabi['name']:<{align}} = {a},")


def alignment(iterable):
    return max((len(x) for x in iterable))


class RootPrinter:
    def __init__(self, out, head, foot):
        self.out, self.head, self.foot = out, head, foot
    def __enter__(self):
        self.out.write("/* Autogenerated by abigen.py, do not edit */\n")
        self.out.write(self.head)
        return self
    def __exit__(self, _, __, ___):
        self.out.write(self.foot)
    def writeln(self, line=""):
        self.out.write(line + "\n")

def header_emit(pr, abi):
    class EnumPrinter:
        def __init__(self, out, name, prefix=None, emit_max=True):
            self.name, self.out, self.emit_max = name, out, emit_max
            self.prefix = prefix if prefix else name.upper()
        def writeln(self, line):
            self.out.writeln("\t" + line)
        def __enter__(self):
            self.out.writeln(f"enum {self.name} {{")
            return self
        def __exit__(self, _, __, ___):
            if self.emit_max:
                self.out.writeln(f"\t__MAX_{self.prefix}")
            self.out.writeln("};\n")
        def emit(self, id):
            self.out.writeln(f"\t{self.prefix}_{id.upper()},")

    class StructPrinter:
        def __init__(self, out, name, member=None):
            self.name, self.out, self.member = name, out, member
        def __enter__(self):
            if self.member:
                self.out.writeln("struct {")
            else:
                self.out.writeln(f"struct {self.name} {{")
            return self
        def __exit__(self, _, __, ___):
            if self.member:
                self.out.writeln(f"}} {self.member};")
            else:
                self.out.writeln("};\n")
        def writeln(self, line):
            self.out.writeln("\t" + line)
        def emit(self, type, member, array=""):
            if not type.endswith("*"):
                type += " "
            self.out.writeln(f"\t{type}{member}{array};")

    pr.writeln("#ifndef _INJECT_PASW_ABI_SPEC")
    pr.writeln("#error \"This file should only be included by pasw-abi.h\"")
    pr.writeln("#endif\n")

    for tabi in abi.each("tables"):
        with StructPrinter(pr, f"pasw_t_{tabi['name']}") as ts:
            ts.emit("struct pasw_table", "table")
            [ ts.emit("struct pasw_field", f) for f in sorted(tabi["fields"]) ]

    with StructPrinter(pr, "pasw_abi") as a:
        with StructPrinter(a, "", member="attr") as attrs:
            for aabi in abi.each("attributes"):
                if "must" in aabi:
                    # Driver requires a particular value at
                    # build time, so there is no need to include
                    # it in the ABI
                    continue

                attrs.emit(aabi.get("type", "u8"), aabi["name"])

        with StructPrinter(a, "", member="var") as vars:
            for vabi in abi.each("variables"):
                vars.emit("struct pasw_var", vabi["name"])

        with StructPrinter(a, "", member="table") as tables:
            for tabi in abi.each("tables"):
                tables.emit(f"const struct pasw_t_{tabi['name']} *", tabi["name"])

        with StructPrinter(a, "", member="stats") as stats:
            stats.emit("size_t", "nr_stats")
            stats.emit("const struct pasw_stat_table *", "table")

if __name__ == "__main__":
    import argparse
    import os
    import sys
    import yaml

    def usage():
        print(f"""{sys.argv[0]} - PASW ABI code generator

Usage: {sys.argv[0]} spec ABI               - Generate ABI struct definition
       {sys.argv[0]} gen NAME ABI REGS CONF - Generate ABI implementation
""")
        sys.exit(1)

    def yml(path):
        return yaml.load(open(path), yaml.SafeLoader)

    def spec(abipath):
        abi = ABI(yml(abipath))
        with RootPrinter(sys.stdout, "", "") as pr:
            header_emit(pr, abi)

    def gen(name, abipath, regspath, confpath):
        abi = ABI(yml(abipath))
        design = Design(name, abi, yml(regspath), yml(confpath))

        with RootPrinter(sys.stdout, "", "") as pr:
            design.emit(pr)

    if len(sys.argv) == 3 and sys.argv[1] == "spec":
        spec(sys.argv[2])
    elif len(sys.argv) == 6 and sys.argv[1] == "gen":
        gen(*sys.argv[2:])
    else:
        usage()
        sys.exit(1)
