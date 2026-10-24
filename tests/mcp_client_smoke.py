"""Interop check with the official MCP Python SDK (tested with mcp 2.3.0).

Run: python tests/mcp_client_smoke.py /path/to/yahat-mcp-example
Requires the optional Python `mcp` package; not part of the C++ build.
"""
import asyncio
import os
import secrets
import socket
import subprocess
import sys

import httpx2
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client


async def check(executable):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    token = secrets.token_urlsafe(24)
    env = dict(os.environ, YAHAT_MCP_EXAMPLE_TOKEN=token)
    process = subprocess.Popen([executable, str(port)], env=env, stdout=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError("MCP example exited before accepting connections")
            try:
                _, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.close()
                await writer.wait_closed()
                break
            except OSError:
                await asyncio.sleep(0.02)
        else:
            raise RuntimeError("MCP example did not start")

        progress = []

        async def on_progress(completed, total, message):
            progress.append((completed, total, message))

        async with httpx2.AsyncClient(headers={"Authorization": f"Bearer {token}"}) as http:
            async with streamable_http_client(f"http://127.0.0.1:{port}/mcp", http_client=http) as streams:
                async with ClientSession(*streams) as session:
                    discovery = await session.discover()
                    assert "2026-07-28" in discovery.supported_versions
                    catalog = await session.list_tools()
                    names = [tool.name for tool in catalog.tools]
                    assert names == sorted(names)
                    assert names == ["demo_greet", "demo_help", "demo_inspect_config"]
                    greeting = await session.call_tool("demo_greet", {"name": "SDK"}, progress_callback=on_progress)
                    assert greeting.structured_content == "Hello, SDK"
                    assert progress and progress[0][0] == 0
                    result = await session.call_tool("demo_inspect_config", {"config": {"mode": "safe"}})
                    assert result.structured_content == {"accepted": True, "mode": "safe"}
                    invalid = await session.call_tool("demo_inspect_config", {"config": {"mode": "invalid"}})
                    assert invalid.is_error
                    help_result = await session.call_tool("demo_help", {"tool": "demo_inspect_config", "topic": "configuration"})
                    assert "fast or safe" in help_result.structured_content["text"]
        print("Official MCP Python SDK: discovery, listing, calls, progress, errors, and help passed")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(check(sys.argv[1]), timeout=20))
