#!/usr/bin/env python3
"""
Checks that ABI 1.x keeps its promise (docs/decisions.md, 8).

1. The three descriptions of the calls agree: wit/kernel.wit, the header
   include/wasm-rtos.h and the kernel's tables (abi.c and every driver's
   OsDriverCall table). Each call is compared by interface, name, the core
   wasm signature it lowers to and the minor version that added it.
2. Every line of every frozen list docs/abi-1.<minor>.txt is still true:
   the call is there with the same signature and minor, the constant has
   the same value. A call or constant in none of the lists fails too: it
   belongs in the list of the minor that adds it.
3. The version is the same in the header, the wit package and os.h, and
   the list of that minor exists.

Usage: tools/check-abi.py [kernel directory]. Exit status 1 on any mismatch.
--print prints every call and constant in the list format instead.
"""

import glob
import os
import re
import sys

ARGS = [arg for arg in sys.argv[1:] if arg != "--print"]
ROOT = os.path.abspath(ARGS[0] if ARGS else os.path.join(os.path.dirname(__file__), ".."))
problems = []


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as file:
        return file.read()


def strip_c_comments(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def minor_of(version):
    return int(version.split(".")[1])


# ---- wit -------------------------------------------------------------------

WIT_WIDE = {"u64", "s64"}
WIT_FLOAT = {"f32": "f", "f64": "F"}


def wit_param_letters(param_type):
    param_type = param_type.strip()
    if param_type == "string" or param_type.startswith("list<"):
        return "ii"
    if param_type in WIT_WIDE:
        return "I"
    return WIT_FLOAT.get(param_type, "i")


def split_params(text):
    params, depth, current = [], 0, ""
    for char in text:
        if char == "<":
            depth += 1
        elif char == ">":
            depth -= 1
        if char == "," and depth == 0:
            params.append(current)
            current = ""
        else:
            current += char
    if current.strip():
        params.append(current)
    return params


def wit_calls():
    text = read("wit/kernel.wit")
    text = re.sub(r"//[^\n]*", "", text)
    package = re.search(r"package\s+wasm-rtos:kernel@(\d+\.\d+\.\d+);", text)
    calls = {}
    for match in re.finditer(r"(@since\(version\s*=\s*([\d.]+)\)\s*)?interface\s+([\w-]+)\s*\{", text):
        since = minor_of(match.group(2)) if match.group(2) else 0
        interface = match.group(3)
        body_start = match.end()
        depth, index = 1, body_start
        while depth:
            depth += {"{": 1, "}": -1}.get(text[index], 0)
            index += 1
        body = text[body_start:index - 1]
        for call in re.finditer(r"(@since\(version\s*=\s*([\d.]+)\)\s*)?(%?[\w-]+)\s*:\s*func\((.*?)\)\s*(->\s*[\w-]+)?\s*;",
                                body, flags=re.S):
            params = "".join(wit_param_letters(param.split(":", 1)[1]) for param in split_params(call.group(4)))
            call_since = minor_of(call.group(2)) if call.group(2) else since
            calls[(interface, call.group(3).lstrip("%"))] = ("i(%s)" % params, call_since)
    return calls, package.group(1) if package else None


# ---- header ----------------------------------------------------------------

C_WIDE = re.compile(r"\b(u?int64_t|long\s+long)\b")


def c_param_letter(param):
    param = param.strip()
    if "*" in param:
        return "i"
    if C_WIDE.search(param):
        return "I"
    if re.search(r"\bdouble\b", param):
        return "F"
    if re.search(r"\bfloat\b", param):
        return "f"
    return "i"


def header_calls_and_constants():
    raw = read("include/wasm-rtos.h")
    text = strip_c_comments(raw)
    calls = {}
    for match in re.finditer(r'WR_IMPORT\("([\w-]+)",\s*"([\w-]+)"\)\s*([^;]*?)\(([^;]*?)\)\s*;', text, flags=re.S):
        params = match.group(4).strip()
        letters = "" if params in ("", "void") else "".join(c_param_letter(p) for p in params.split(","))
        result = match.group(3)
        calls[(match.group(1), match.group(2))] = ("%s(%s)" % ("v" if re.search(r"\bvoid\b", result) else
                                                              c_param_letter(result), letters), None)
    constants = {}
    for match in re.finditer(r"^#define\s+(WR_[A-Z0-9_]+)[ \t]+([^\n]*?)\s*$", text, flags=re.M):
        constants[match.group(1)] = re.sub(r"\s+", " ", match.group(2))
    import_module = re.search(r'import_module\("wasm-rtos:kernel/" interface "@(\d+\.\d+\.\d+)"\)', raw)
    return calls, constants, import_module.group(1) if import_module else None


# Defines that are not part of what a program relies on value by value
# (the header's own helpers end in an underscore).
NOT_FROZEN = {"WR_ABI_VERSION_MINOR"}


def frozen_constant(constant):
    return constant not in NOT_FROZEN and not constant.endswith("_")


# ---- kernel ----------------------------------------------------------------

def kernel_calls():
    calls = {}
    abi = strip_c_comments(read("abi.c"))
    table = re.search(r"g_abi_imports\[\]\s*=\s*\{(.*?)\n\};", abi, flags=re.S).group(1)
    for match in re.finditer(r'\{\s*WR_MODULE\("([\w-]+)"\),\s*"([\w-]+)",\s*"([^"]+)",\s*\w+,\s*(\d+)U?\s*\}', table):
        calls[(match.group(1), match.group(2))] = (match.group(3), int(match.group(4)))
    for path in sorted(glob.glob(os.path.join(ROOT, "*.c"))):
        text = strip_c_comments(read(os.path.basename(path)))
        for table in re.finditer(r"static const OsDriverCall (\w+)\[\]\s*=\s*\{(.*?)\n\};", text, flags=re.S):
            driver = re.search(r"\.interface\s*=\s*\"([\w-]+)\",(?:(?!\};).)*?\.calls\s*=\s*%s\b" % table.group(1),
                               text, flags=re.S)
            if driver is None:
                problems.append("%s: no driver uses %s" % (os.path.basename(path), table.group(1)))
                continue
            for match in re.finditer(r'\{\s*"([\w-]+)",\s*"([^"]+)",\s*\w+,\s*(\d+)U?\s*\}', table.group(2)):
                calls[(driver.group(1), match.group(1))] = (match.group(2), int(match.group(3)))
    return calls


def kernel_version():
    text = read("os.h")
    major = re.search(r"#define OS_ABI_VERSION_MAJOR (\d+)U", text).group(1)
    minor = re.search(r"#define OS_ABI_VERSION_MINOR (\d+)U", text).group(1)
    return "%s.%s.0" % (major, minor)


# ---- frozen lists ----------------------------------------------------------

def frozen_lists():
    calls, constants = {}, {}
    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "abi-1.*.txt"))):
        name = os.path.relpath(path, ROOT)
        with open(path, encoding="utf-8") as file:
            for number, line in enumerate(file, 1):
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                fields = line.split(None, 2)
                where = "%s:%d" % (name, number)
                if fields[0] == "call" and len(fields) == 3 and len(fields[2].split()) == 3:
                    interface = fields[1]
                    call, signature, since = fields[2].split()
                    calls[(interface, call)] = (signature, int(since), where)
                elif fields[0] == "const" and len(fields) == 3:
                    constants[fields[1]] = (fields[2], where)
                else:
                    problems.append("%s: not 'call <interface> <name> <signature> <minor>' or 'const <name> <value>'"
                                    % where)
    return calls, constants


def name(key):
    return "%s.%s" % key


def main():
    wit, wit_version = wit_calls()
    header, constants, header_version = header_calls_and_constants()
    kernel = kernel_calls()
    version = kernel_version()

    for label, other in (("wit package", wit_version), ("header imports", header_version)):
        if other != version:
            problems.append("version: os.h says %s, %s says %s" % (version, label, other))
    header_minor = constants.get("WR_ABI_VERSION_MINOR")
    if header_minor != version.split(".")[1]:
        problems.append("version: os.h says %s, WR_ABI_VERSION_MINOR is %s" % (version, header_minor))
    if not os.path.exists(os.path.join(ROOT, "docs", "abi-1.%s.txt" % version.split(".")[1])):
        problems.append("docs/abi-1.%s.txt is missing: every minor version lists what it adds" % version.split(".")[1])

    for key in sorted(set(wit) | set(header) | set(kernel)):
        if key not in kernel:
            problems.append("%s: in %s but not in the kernel" % (name(key), "wit" if key in wit else "the header"))
            continue
        signature, since = kernel[key]
        if key not in wit:
            problems.append("%s: in the kernel but not in wit/kernel.wit" % name(key))
        elif wit[key] != (signature, since):
            problems.append("%s: kernel %s since 1.%d, wit %s since 1.%d" % ((name(key), signature, since) + wit[key]))
        if key not in header:
            problems.append("%s: in the kernel but not in include/wasm-rtos.h" % name(key))
        elif header[key][0] != signature:
            problems.append("%s: kernel %s, header %s" % (name(key), signature, header[key][0]))

    frozen_calls, frozen_constants = frozen_lists()
    for key, (signature, since, where) in sorted(frozen_calls.items()):
        if key not in kernel:
            problems.append("%s: %s is frozen but gone from the kernel" % (where, name(key)))
        elif kernel[key] != (signature, since):
            problems.append("%s: %s is frozen as %s since 1.%d, the kernel has %s since 1.%d"
                            % ((where, name(key), signature, since) + kernel[key]))
    for key in sorted(set(kernel) - set(frozen_calls)):
        problems.append("%s: not in any docs/abi-1.*.txt; add it to the list of the minor that adds it" % name(key))
    for constant, (value, where) in sorted(frozen_constants.items()):
        if constant not in constants:
            problems.append("%s: %s is frozen but gone from the header" % (where, constant))
        elif constants[constant] != value:
            problems.append("%s: %s is frozen as %s, the header has %s" % (where, constant, value, constants[constant]))
    for constant in sorted(c for c in set(constants) - set(frozen_constants) if frozen_constant(c)):
        problems.append("%s: not in any docs/abi-1.*.txt; add it to the list of the minor that adds it" % constant)

    for problem in problems:
        print("FAIL " + problem)
    if not problems:
        print("ok   ABI %s: %d calls and %d constants agree with wit, the header and docs/abi-1.*.txt"
              % (version, len(kernel), len(frozen_constants)))
    return 1 if problems else 0


def write_list():
    """Prints every call and constant as list lines; docs/abi-1.8.txt was made this way."""
    for key, (signature, since) in sorted(kernel_calls().items(), key=lambda item: (item[1][1], item[0])):
        print("call %s %s %s %d" % (key[0], key[1], signature, since))
    for constant, value in header_calls_and_constants()[1].items():
        if frozen_constant(constant):
            print("const %s %s" % (constant, value))


if __name__ == "__main__":
    if "--print" in sys.argv[1:]:
        write_list()
        sys.exit(0)
    sys.exit(main())
