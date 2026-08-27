#!/usr/bin/env python3
"""Generate the typed eth_*/op_* wrappers from the library's own dispatch table.

The wrappers are three lines each over one shared `rpc()` path, so hand-writing
sixty-one of them would be sixty-one chances to transpose an argument. They are
generated instead — but COMMITTED as literal text, because the module's code
generator parses verified_proxy_impl.h as TEXT to build the LIDL contract and
would not see anything hidden behind a macro.

Run with --check in CI to prove the committed blocks still match.

The table below was extracted mechanically from nimbus-eth1's
nimbus_verified_proxy/library/c_frontend.nim (the `case $name` inside proxyCall)
at the revision this module pins. Re-extract with --from-source <c_frontend.nim>
after bumping that input.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"

BEGIN = "// BEGIN GENERATED RPC WRAPPERS -- edit tools/gen_rpc_methods.py, not this"
END = "// END GENERATED RPC WRAPPERS"

# Parameter names, per RPC method, in order. Only the eth_ spellings are listed;
# the op_ mirror reuses them. Names reach the LIDL contract and `lm methods`, so
# they are the API's documentation as much as its signature.
PARAM_NAMES = {
    "getBalance": ["address", "blockTag"],
    "getCode": ["address", "blockTag"],
    "getTransactionCount": ["address", "blockTag"],
    "getStorageAt": ["address", "slot", "blockTag"],
    "getBlockByNumber": ["blockTag", "fullTransactions"],
    "getBlockByHash": ["blockHash", "fullTransactions"],
    "getUncleCountByBlockNumber": ["blockTag"],
    "getUncleCountByBlockHash": ["blockHash"],
    "getBlockTransactionCountByNumber": ["blockTag"],
    "getBlockTransactionCountByHash": ["blockHash"],
    "getTransactionByBlockNumberAndIndex": ["blockTag", "index"],
    "getTransactionByBlockHashAndIndex": ["blockHash", "index"],
    "getTransactionByHash": ["txHash"],
    "getTransactionReceipt": ["txHash"],
    "getBlockReceipts": ["blockTag"],
    "call": ["txArgs", "blockTag", "optimisticStateFetch"],
    "estimateGas": ["txArgs", "blockTag", "optimisticStateFetch"],
    "createAccessList": ["txArgs", "blockTag", "optimisticStateFetch"],
    "getLogs": ["filterOptions"],
    "newFilter": ["filterOptions"],
    "uninstallFilter": ["filterId"],
    "getFilterLogs": ["filterId"],
    "getFilterChanges": ["filterId"],
    "feeHistory": ["blockCount", "newestBlock", "rewardPercentiles"],
    "sendRawTransaction": ["txHexBytes"],
}

# The one JSON parameter that is an ARRAY rather than an object. Everything else
# reaching the library as a raw JSON node is a transaction or filter object.
JSON_LISTS = {("feeHistory", 2)}

CPP_TYPE = {
    "str": "const std::string& ",
    "bool": "bool ",
    "u64": "uint64_t ",
}

# Methods this module drives itself; a typed wrapper would invite callers to
# fight the runtime for control of them.
SKIP = {"eth_syncing"}


def extract(path):
    body = Path(path).read_text()
    body = body[body.index("proc proxyCall("):]
    out = []
    for block in re.split(r'\n  of "', body)[1:]:
        name = block.split('"', 1)[0]
        seg = block.split('\n  of "')[0]
        kinds = []
        for m in re.finditer(
            r"parsedParams\[(\d+)\]\.(getStr|getBool|getBiggestInt)\(\)"
            r"|\(\$parsedParams\[(\d+)\]\)", seg):
            if m.group(2):
                kinds.append((int(m.group(1)),
                              {"getStr": "str", "getBool": "bool",
                               "getBiggestInt": "u64"}[m.group(2)]))
            else:
                kinds.append((int(m.group(3)), "json"))
        out.append({"rpc": name, "params": [k for _, k in sorted(set(kinds))]})
    return out


def cpp_name(rpc):
    prefix, rest = rpc.split("_", 1)
    return prefix + rest[0].upper() + rest[1:]


def stem(rpc):
    return rpc.split("_", 1)[1]


def signature(rpc, params):
    base = stem(rpc)
    names = PARAM_NAMES.get(base, [f"arg{i}" for i in range(len(params))])
    args = []
    for i, kind in enumerate(params):
        if kind == "json":
            t = "const LogosList& " if (base, i) in JSON_LISTS else "const LogosMap& "
        else:
            t = CPP_TYPE[kind]
        args.append(t + names[i])
    return f"StdLogosResult {cpp_name(rpc)}({', '.join(args)})"


def doc(rpc, params):
    base = stem(rpc)
    lines = [f"/// `{rpc}`, verified."]
    if base in ("call", "estimateGas", "createAccessList"):
        lines.append("///")
        lines.append("/// `optimisticStateFetch` is upstream's own extension to the standard")
        lines.append("/// JSON-RPC signature, not a parameter callers will know from elsewhere.")
    if rpc.startswith("op_"):
        lines.append("///")
        lines.append("/// Requires an OP-Stack network and `opExecutionApiUrls`; otherwise the")
        lines.append("/// library answers with a clear error rather than a wrong value.")
    return lines


def emit_header(table):
    out = [BEGIN]
    for e in table:
        out += ["    " + l for l in doc(e["rpc"], e["params"])]
        out.append(f"    {signature(e['rpc'], e['params'])};")
        out.append("")
    out.append("    " + END)
    return "\n".join(out)


def emit_impl(table):
    out = [BEGIN]
    for e in table:
        base, params = stem(e["rpc"]), e["params"]
        names = PARAM_NAMES.get(base, [f"arg{i}" for i in range(len(params))])
        sig = signature(e["rpc"], params).replace(
            "StdLogosResult ", "StdLogosResult VerifiedProxyImpl::", 1)
        args = ", ".join(names)
        out.append(f"{sig} {{")
        out.append(f'    return rpc("{e["rpc"]}", json::array({{{args}}}));')
        out.append("}")
        out.append("")
    out.append(END)
    return "\n".join(out)


def splice(path, block):
    text = Path(path).read_text()
    i, j = text.index(BEGIN), text.index(END) + len(END)
    return text[:i] + block.strip() + text[j:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from-source", help="path to c_frontend.nim to re-extract the table")
    ap.add_argument("--check", action="store_true", help="fail if the committed files differ")
    a = ap.parse_args()

    tbl_path = HERE / "rpc_methods.json"
    if a.from_source:
        table = extract(a.from_source)
        tbl_path.write_text(json.dumps(table, indent=1) + "\n")
    table = json.loads(tbl_path.read_text())
    table = [e for e in table if e["rpc"] not in SKIP]

    targets = {
        SRC / "verified_proxy_impl.h": emit_header(table),
        SRC / "verified_proxy_rpc.cpp": emit_impl(table),
    }
    bad = False
    for path, block in targets.items():
        new = splice(path, block)
        if a.check:
            if new != path.read_text():
                print(f"DRIFT: {path.name} does not match the generator", file=sys.stderr)
                bad = True
        else:
            path.write_text(new)
    if a.check and bad:
        print("run: python3 tools/gen_rpc_methods.py", file=sys.stderr)
        return 1
    print(f"{len(table)} wrappers {'checked' if a.check else 'generated'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
