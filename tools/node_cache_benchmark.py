"""Linux TCP/TLS cache A/B runner. Both binaries must come from Release builds.

Uses synthetic configuration and an isolated loopback origin, but the production
data_plane/http_session/cache code. Each request opens a TCP connection because
the current data plane sends Connection: close. No external service is needed.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import select
import statistics
import subprocess
import tempfile


def line(process):
    if not select.select([process.stdout], [], [], 30)[0]:
        raise TimeoutError("benchmark process did not answer in 30 seconds")
    value = process.stdout.readline().strip()
    if not value:
        raise RuntimeError(f"benchmark process exited: {process.poll()}")
    return value


def start(binary, cpus, args, log):
    process = subprocess.Popen(
        ["taskset", "-c", cpus, str(binary), *map(str, args)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
    )
    try:
        value = line(process)
        if not value.startswith("READY "):
            raise RuntimeError(value)
        return process, int(value.split()[1])
    except Exception:
        stop(process)
        raise


def stop(process):
    if process is None:
        return
    try:
        process.communicate("quit\n", timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def origin_hits(process):
    process.stdin.write("stats\n")
    process.stdin.flush()
    return int(line(process))


def usage(process):
    fields = Path(f"/proc/{process.pid}/stat").read_text().split()
    cpu = (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK")
    status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
    rss = next(int(row.split()[1]) for row in status if row.startswith("VmHWM:"))
    return cpu, rss


def load(binary, port, objects, size, requests, concurrency, prefix, hit,
         transport="http", coding="identity", pattern="repeat"):
    options = [transport, coding, pattern] if (transport, coding, pattern) != ("http", "identity", "repeat") else []
    result = subprocess.run(
        ["taskset", "-c", "4-11", str(binary), "load", str(port), str(objects),
         str(size), str(requests), str(concurrency), prefix, "hit" if hit else "any", *options],
        check=True, capture_output=True, text=True, timeout=180,
    )
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--scenario", action="append", help="Run only the named scenario; may be repeated")
    parser.add_argument("--transport", choices=["http", "tls12", "tls13"], default="http")
    parser.add_argument("--coding", choices=["identity", "gzip", "br", "zstd"], action="append")
    parser.add_argument("--pattern", choices=["repeat", "mixed"], default="repeat")
    parser.add_argument("--requests", type=int, help="Override request count for hit scenarios")
    parser.add_argument("--cold-objects", type=int, default=1024)
    parser.add_argument("--compression", action="store_true", help="Enable compression even for identity control")
    args = parser.parse_args()
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    codings = args.coding or ["identity"]
    compression = args.compression or any(coding != "identity" for coding in codings)
    # Identical object counts, bytes, request counts, concurrency, CPUs and client
    # executable. Alternate order per trial to reduce temperature/order bias.
    scenarios = [
        ("memory_single", "memory", 1, 4096, 100000, 32, 0, True),
        ("memory_2048", "memory", 2048, 4096, 20000, 32, 0, True),
        ("memory_256", "memory", 256, 65536, 10000, 32, 0, True),
        ("disk_256", "disk", 256, 65536, 30000, 32, 0, True),
        ("tiered_pressure", "tiered", 128, 65536, 20000, 32, 0, True),
        ("disk_fill", "disk", args.cold_objects, 65536, args.cold_objects, 16, 0, False),
        ("cold_burst", "memory", 1, 4096, 64, 64, 600, False),
    ]
    if args.requests:
        scenarios = [(*row[:4], args.requests if row[7] else row[4], *row[5:]) for row in scenarios]
    if args.scenario:
        names = {row[0] for row in scenarios}
        if set(args.scenario) - names:
            parser.error("Unknown scenario")
        scenarios = [row for row in scenarios if row[0] in args.scenario]
    results = []
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="flexedge-cache-identity-") as shared_identity, (args.output / "processes.log").open("w") as log:
        identity = Path(shared_identity)
        subprocess.run([str(binaries["after"]), "identity", str(identity)], check=True, stderr=log)
        for name, storage, objects, size, requests, concurrency, delay, hit in scenarios:
            for coding in codings:
                case = name if args.transport == "http" and coding == "identity" else f"{args.transport}_{coding}_{name}"
                for trial in range(args.trials):
                    for version in (["before", "after"] if trial % 2 == 0 else ["after", "before"]):
                        origin = server = None
                        with tempfile.TemporaryDirectory(prefix="flexedge-cache-benchmark-") as directory:
                            try:
                                origin, source_port = start(binaries["after"], "12", ["origin", size, delay, args.pattern], log)
                                server, port = start(binaries[version], "0-3", ["serve", source_port, storage, directory,
                                                            args.transport, "on" if compression else "off", identity], log)
                                if hit:
                                    load(binaries["after"], port, objects, size, objects, 1, "/warm/", False,
                                         args.transport, coding, args.pattern)
                                    load(binaries["after"], port, objects, size, objects, 1, "/warm/", True,
                                         args.transport, coding, args.pattern)
                                before_hits = origin_hits(origin)
                                cpu_start, _ = usage(server)
                                result = load(binaries["after"], port, objects, size, requests, concurrency,
                                              "/warm/" if hit else "/cold/", hit, args.transport, coding, args.pattern)
                                cpu_end, rss = usage(server)
                                result.update(scenario=name, case=case, version=version, trial=trial + 1, objects=objects,
                                              bytes=size, concurrency=concurrency, transport=args.transport, coding=coding, pattern=args.pattern,
                                              origin_requests=origin_hits(origin) - before_hits, cpu_seconds=cpu_end - cpu_start, peak_rss_kib=rss)
                                if hit and result["origin_requests"]:
                                    raise RuntimeError("hit workload unexpectedly reached origin")
                                if args.transport != "http" and result["tls_full_handshakes"] != requests:
                                    raise RuntimeError("missing full TLS handshakes")
                                if name == "disk_fill" and result["origin_requests"] != requests:
                                    raise RuntimeError("cold fill workload did not reach origin once per object")
                                if name == "cold_burst" and version == "after" and result["origin_requests"] != 1:
                                    raise RuntimeError("concurrent requests did not coalesce into one fill")
                                results.append(result)
                                print(json.dumps(result), flush=True)
                                (args.output / "raw.json").write_text(json.dumps(results, indent=2) + "\n")
                            finally:
                                stop(server)
                                stop(origin)
    summary = {}
    for name in dict.fromkeys(row["case"] for row in results):
        summary[name] = {}
        for version in binaries:
            rows = [r for r in results if r["case"] == name and r["version"] == version]
            summary[name][version] = {field: statistics.median(r[field] for r in rows)
                                     for field in ("rps", "mib_s", "p50_ms", "p95_ms", "p99_ms", "cpu_seconds", "peak_rss_kib", "origin_requests", "errors",
                                                   "http_mib_s", "encoded_mib_s", "mean_encoded_body_bytes", "tls_full_handshakes")}
        summary[name]["rps_change_percent"] = 100 * (summary[name]["after"]["rps"] / summary[name]["before"]["rps"] - 1)
        pairs = [{version: next(r for r in results if r["case"] == name and r["version"] == version and r["trial"] == trial)
                  for version in binaries} for trial in range(1, args.trials + 1)]
        summary[name]["paired_rps_changes_percent"] = [100 * (pair["after"]["rps"] / pair["before"]["rps"] - 1) for pair in pairs]
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    (args.output / "environment.json").write_text(json.dumps({
        "platform": platform.platform(), "compiler": subprocess.getoutput("g++ --version").splitlines()[0],
        "cpu": subprocess.getoutput("lscpu"), "server_cpus": "0-3", "client_cpus": "4-11", "origin_cpu": "12",
        "trials": args.trials, "binaries": {k: str(v) for k, v in binaries.items()},
        "transport": args.transport, "compression": compression, "codings": codings, "pattern": args.pattern,
        "notes": "WSL2; Linux ext4; warm OS page cache; two data workers; HTTP/1.1 Connection: close; TLS uses RSA-2048 test identity, full handshakes, no certificate verification; identical identity for every sample; http_bytes excludes TLS records and handshakes",
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
