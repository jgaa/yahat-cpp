#!/usr/bin/env python3
"""Build and run native tests and the pinned official MCP server scenarios."""
import argparse
import collections
import datetime
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
TOOLING = ROOT / "tests/mcp_conformance"
PROTOCOL = "2026-07-28"


def command(args, log, *, cwd=ROOT, timeout=600, verbose=False):
    """Keep every command and its output; stop the entire child group on timeout."""
    with log.open("w") as output:
        output.write("Command: " + repr([str(arg) for arg in args]) + "\n")
        output.flush()
        process = subprocess.Popen(args, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            stop(process)
            raise
    if verbose or code:
        print(log.read_text(errors="replace")[-16000:])
    return code


def stop(process):
    # Kill descendants too, including Node helpers, on failure or interruption.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def rpc(port, method, params=None):
    params = dict(params or {})
    params["_meta"] = {"io.modelcontextprotocol/protocolVersion": PROTOCOL,
                       "io.modelcontextprotocol/clientCapabilities": {}}
    message = {"jsonrpc": "2.0", "id": "readiness", "method": method, "params": params}
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        headers = {
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": PROTOCOL, "Mcp-Method": method}
        if method == "tools/call":
            headers["Mcp-Name"] = params["name"]
        connection.request("POST", "/mcp", json.dumps(message), headers)
        response = connection.getresponse()
        raw = response.read().decode()
        if "text/event-stream" in response.getheader("Content-Type", ""):
            frames = [json.loads(line[5:].strip()) for line in raw.splitlines() if line.startswith("data:")]
            replies = [frame for frame in frames if frame.get("id") == "readiness"]
            if len(replies) != 1:
                raise RuntimeError("Expected one final MCP response on the SSE stream")
            return response.status, replies[0]
        return response.status, json.loads(raw)
    finally:
        connection.close()


def fixture_checks(port, documentation):
    """Exercise yahat-specific public API examples over the real HTTP endpoint."""
    def result(name, arguments):
        status, body = rpc(port, "tools/call", {"name": name, "arguments": arguments})
        if status != 200 or "result" not in body or body["result"].get("isError"):
            raise RuntimeError(f"Fixture tool failed: {name}: {body}")
        return body["result"]["structuredContent"]

    expected = [("echo", {"text": "hello"}, "hello"),
                ("add", {"a": 1.5, "b": 2}, 3.5),
                ("optional", {}, None), ("optional", {"value": None}, None),
                ("optional", {"value": 7}, 7),
                ("structured", {"items": [{"x": 3}]}, {"items": [{"x": 3}], "count": 1}),
                ("member", {"text": "owned"}, "fixture:owned")]
    for name, arguments, value in expected:
        if result(name, arguments) != value:
            raise RuntimeError(f"Incorrect fixture result for {name}")
    status, listing = rpc(port, "tools/list")
    text = json.dumps(listing)
    if status != 200 or "EXTERNAL_DOCUMENTATION" in text or len(text) >= documentation.stat().st_size:
        raise RuntimeError("Progressive documentation inflated tools/list")
    overview = result("conformance_help", {"tool": "structured"})
    if set(overview["topics"]) != {"embedded", "external"}:
        raise RuntimeError("Documentation topics disagree with fixture metadata")
    embedded = result("conformance_help", {"tool": "structured", "topic": "embedded"})
    if "integer values" not in embedded["text"]:
        raise RuntimeError("Embedded documentation missing")
    external = result("conformance_help", {"tool": "structured", "topic": "external"})
    if external["text"] != documentation.read_text():
        raise RuntimeError("External documentation was not loaded")
    documentation.unlink()
    _, missing = rpc(port, "tools/call", {"name": "conformance_help", "arguments": {"tool": "structured", "topic": "external"}})
    if not missing.get("result", {}).get("isError"):
        raise RuntimeError("Missing external documentation did not produce a tool error")
    return {"status": "passed", "tool_checks": len(expected), "discovery_bytes": len(text.encode()),
            "external_document_bytes": len(external["text"].encode()), "help": ["embedded", "external", "missing"]}


def run(args, logs, summary):
    native = not args.conformance_only
    official = not args.native_only
    for dependency in (["cmake", "ctest"] if native else ["cmake"]) + (["node", "npm"] if official else []):
        if not shutil.which(dependency):
            raise RuntimeError(f"Missing dependency: {dependency}; see docs/mcp-testing.md")
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    if command(["cmake", "-S", ROOT, "-B", build, "-DYAHAT_ENABLE_MCP=ON",
                "-DYAHAT_WITH_TESTS=ON"], logs / "configure.log", verbose=args.verbose):
        raise RuntimeError("CMake configuration failed (Boost, OpenSSL, zlib and GTest are required)")
    targets = (["http_tests_tests", "jsonrpc_tests_tests", "mcp_tests_tests"] if native else [])
    if official:
        targets.append("yahat-mcp-test-server")
    if command(["cmake", "--build", build, "--parallel", str(args.jobs), "--target", *targets],
               logs / "build.log", verbose=args.verbose):
        raise RuntimeError("Build failed")
    failed = False
    if native:
        code = command(["ctest", "--test-dir", build, "--output-on-failure", "--timeout", "60",
                        "-R", "^(Http|JsonRpc|Mcp|InstalledPackage)Test$", "--output-junit", logs / "native.xml"],
                       logs / "native.log", verbose=args.verbose)
        summary["native"] = {"status": "failed" if code else "passed", "exit_code": code}
        failed |= bool(code)
        junit = ET.parse(logs / "native.xml").getroot()
        cases = junit.findall("testcase")
        if {case.get("name") for case in cases} != {"HttpTest", "JsonRpcTest", "McpTest", "InstalledPackageTest"}:
            raise RuntimeError("Expected three native test executables and the installed package test; inspect native.xml")
        count = sum(int(n) for case in cases for n in re.findall(r"\[==========\] Running (\d+) tests?", case.findtext("system-out", "")))
        summary["native"]["executables"] = 3
        installed = next(case for case in cases if case.get("name") == "InstalledPackageTest")
        summary["native"]["installed_package"] = "passed" if installed.get("status") == "run" and installed.find("failure") is None else "failed"
        summary["native"]["tests"] = count
        print(f"Native tests: {summary['native']['status']} ({count} GTests and installed package consumer)", flush=True)
    if not official:
        return int(failed)
    version = subprocess.check_output(["node", "--version"], text=True).strip()
    if int(version.lstrip("v").split(".")[0]) < 22:
        raise RuntimeError(f"Node.js >=22 required; found {version}")
    npm_dir = build / "mcp-conformance"
    # npm ci verifies package-lock integrity, with no global installation or lifecycle scripts.
    if command(["npm", "ci", "--prefix", npm_dir, "--ignore-scripts", "--no-audit", "--no-fund", "--silent"],
               logs / "npm.log", verbose=args.verbose):
        raise RuntimeError("Pinned conformance installation failed; see npm.log")
    executable = npm_dir / "node_modules/.bin/conformance"
    package = json.loads((npm_dir / "node_modules/@modelcontextprotocol/conformance/package.json").read_text())
    expected = json.loads((npm_dir / "package.json").read_text())["dependencies"]["@modelcontextprotocol/conformance"]
    if package["version"] != expected:
        raise RuntimeError("Installed conformance version does not match the pin")
    summary["framework_version"] = expected
    if command([executable, "server", "--help"], logs / "runner-help.log"):
        raise RuntimeError("Cannot inspect conformance runner")
    if "--spec-version" not in (logs / "runner-help.log").read_text():
        raise RuntimeError("Conformance runner lacks explicit revision selection")
    if command([executable, "list", "--server"], logs / "scenarios.log"):
        raise RuntimeError("Cannot enumerate conformance scenarios")
    inventory = dict(re.findall(r"^  - ([\w-]+) \[([^\]]+)\]", (logs / "scenarios.log").read_text(), re.M))
    manifest = json.loads((TOOLING / "scenarios.json").read_text())
    selected = manifest["run"]
    classified = set(selected) | set(manifest["inapplicable"])
    if not inventory or set(inventory) != classified or len(selected) != len(set(selected)) or set(selected) & set(manifest["inapplicable"]):
        raise RuntimeError(f"Scenario inventory changed; review classifications: {sorted(set(inventory) ^ classified)}")
    for name in selected:
        if PROTOCOL not in inventory[name].split(","):
            raise RuntimeError(f"Selected scenario {name} does not target {PROTOCOL}")
    if manifest["protocol"] != PROTOCOL or f'protocol_version = "{PROTOCOL}"' not in (ROOT / "include/yahat/Mcp.h").read_text():
        raise RuntimeError("Test target revision differs from the manifest or implementation")
    summary["inapplicable"] = manifest["inapplicable"]
    if args.scenario:
        if args.scenario not in selected:
            raise RuntimeError(f"Scenario is not applicable: {args.scenario}; see scenarios.json")
        selected = [args.scenario]
    if args.port:
        port = args.port
    else:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
    # Refuse an occupied port so a previous or production server is never tested.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", port))
    documentation = logs / "fixture-help.txt"
    documentation.write_text("EXTERNAL_DOCUMENTATION " + "details " * 2500)
    fixture = args.executable.resolve() if args.executable else build / "bin/yahat-mcp-test-server"
    with (logs / "server.log").open("w") as output:
        process = subprocess.Popen([fixture, str(port), documentation], stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 15
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"Test server exited with {process.returncode}; see server.log")
                try:
                    status, body = rpc(port, "server/discover")
                    if status != 200 or body.get("id") != "readiness" or PROTOCOL not in body["result"]["supportedVersions"] or body["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] != "conformance":
                        raise RuntimeError("Readiness failed: endpoint is not the expected MCP test fixture")
                    break
                except (OSError, http.client.HTTPException):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("MCP server startup timed out; see server.log")
                    time.sleep(0.05)
            if not args.scenario:
                summary["fixture"] = fixture_checks(port, documentation)
                (logs / "fixture-checks.json").write_text(json.dumps(summary["fixture"], indent=2) + "\n")
                print("Fixture API and external documentation: passed", flush=True)
            summary["official"] = []
            for name in selected:
                if process.poll() is not None:
                    raise RuntimeError("Test server crashed; see server.log")
                scenario_logs = logs / name
                scenario_logs.mkdir()
                code = command([executable, "server", "--url", f"http://127.0.0.1:{port}/mcp",
                                "--scenario", name, "--spec-version", PROTOCOL,
                                "--expected-failures", TOOLING / "baseline.yml",
                                "--output-dir", scenario_logs, "--timeout", "15000"],
                               scenario_logs / "runner.log", cwd=logs, timeout=45, verbose=args.verbose)
                # Official check files are retained unchanged; expose skipped checks and failure details.
                checks = []
                for file in scenario_logs.rglob("checks.json"):
                    data = json.loads(file.read_text())
                    checks.extend(data if isinstance(data, list) else data.get("checks", []))
                if not checks:
                    raise RuntimeError(f"No official checks produced for {name}; see its runner.log")
                counts = dict(collections.Counter(check["status"] for check in checks))
                entry = {"scenario": name, "exit_code": code, "checks": counts,
                         "failures": [c for c in checks if c["status"] == "FAILURE"],
                         "skipped": [c for c in checks if c["status"] == "SKIPPED"]}
                entry["status"] = "failed" if code else "expected failures" if counts.get("FAILURE") else "passed"
                summary["official"].append(entry)
                print(f"Official {name}: {entry['status']} {counts}", flush=True)
                failed |= bool(code)
        finally:
            stop(process)
        if process.returncode != 0:
            raise RuntimeError(f"Fixture did not shut down cleanly (exit {process.returncode}); see server.log")
    totals = collections.Counter(entry["status"] for entry in summary["official"])
    summary["official_totals"] = dict(totals)
    summary["expected_failure_checks"] = sum(len(entry["failures"]) for entry in summary["official"] if entry["status"] == "expected failures")
    print(f"Official scenarios: {dict(totals)}; expected failure checks: {summary['expected_failure_checks']}")
    print(f"Inapplicable scenarios: {len(manifest['inapplicable'])} (reasons in scenarios.json and summary.json)")
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--native-only", action="store_true")
    mode.add_argument("--conformance-only", action="store_true")
    parser.add_argument("--scenario", help="Run one applicable official scenario")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/mcp-tests")
    parser.add_argument("--executable", type=Path, help="Override the test server executable")
    parser.add_argument("--port", type=int, choices=range(1, 65536), metavar="PORT")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or (args.native_only and args.scenario):
        parser.error("--jobs must be positive; --scenario requires conformance tests")
    logs_root = ROOT / "tests/mcp-results"
    logs_root.mkdir(exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix=datetime.datetime.now().strftime("%Y%m%d-%H%M%S-"), dir=logs_root)).resolve()
    summary = {"protocol": PROTOCOL, "logs": str(logs)}
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    code = 1
    try:
        code = run(args, logs, summary)
    except KeyboardInterrupt:
        summary["error"] = "Interrupted"
        code = 130
    except Exception as error:
        summary["error"] = str(error)
        print(f"ERROR: {error}", file=sys.stderr)
    finally:
        summary["exit_code"] = code
        (logs / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(f"{'PASS' if code == 0 else 'FAIL'} — logs: {logs}")
    return code


if __name__ == "__main__":
    sys.exit(main())
