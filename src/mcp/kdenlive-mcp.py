#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive MCP prototype contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local stdio bridge to an explicitly enabled Kdenlive MCP socket.

Python 3.10+; standard library only. Windows accepts a local Qt socket name;
Unix accepts an absolute AF_UNIX path. There is no network transport, process
execution, automatic reconnect, or request replay.

A single daemon worker owns blocking pipe/socket I/O. The main thread enforces
one deadline for connection, send and response. An expired transaction terminates
the CLI without waiting for that worker; the OS closes its handles on process
exit. This bridge is a CLI process, not an embeddable long-running service.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import queue
import re
import socket
import struct
import sys
import threading
import time
from typing import BinaryIO, Callable, Protocol, TextIO

MAX_FRAME_BYTES = 1024 * 1024
# Keep the bound explicit: Python 3.15's iterative decoder no longer rejects
# deeply nested input through the interpreter's recursion limit.
MAX_JSON_DEPTH = 1024
# Reserve enough room to return a bounded local failure with the original ID.
MAX_ID_BYTES = MAX_FRAME_BYTES - 2048
READ_CHUNK_BYTES = 65536
ERROR_PIPE_BUSY = 231


class Transport(Protocol):
    def read(self, count: int) -> bytes: ...
    def write(self, data: bytes) -> None: ...
    def close(self) -> None: ...


class ProtocolFailure(Exception):
    pass


class BridgeFailure(Exception):
    def __init__(self, message: str, uncertain: bool):
        super().__init__(message)
        self.uncertain = uncertain


def load_json(data: bytes) -> object:
    depth = 0
    quoted = escaped = False
    for byte in data:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:  # backslash
                escaped = True
            elif byte == 34:  # quote
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):  # array/object start
            depth += 1
            if depth > MAX_JSON_DEPTH:
                raise ValueError("JSON nesting exceeds the local protocol limit.")
        elif byte in (93, 125):
            depth -= 1
    def invalid_constant(value: str) -> None:
        raise ValueError("Non-finite JSON number")
    return json.loads(data.decode("utf-8"), parse_constant=invalid_constant)


def valid_id(value: object) -> bool:
    return (value is None or isinstance(value, str)
            or (type(value) in (int, float)
                and (not isinstance(value, float) or math.isfinite(value))))


def id_fits(value: object) -> bool:
    if isinstance(value, str) and len(value.encode("utf-16-le", "surrogatepass")) > 256:
        return False  # Match the native endpoint's 128 UTF-16-character ID limit.
    return len(json.dumps(value, ensure_ascii=True, allow_nan=False).encode("ascii")) <= MAX_ID_BYTES


def request_error(value: object) -> str | None:
    if not isinstance(value, dict):
        return "Expected one JSON-RPC object; batches are unsupported."
    if value.get("jsonrpc") != "2.0" or not isinstance(value.get("method"), str):
        return "Expected JSON-RPC 2.0 and a string method."
    if "id" in value and not valid_id(value["id"]):
        return "Invalid JSON-RPC request ID."
    if "id" in value and not id_fits(value["id"]):
        return "Request ID leaves insufficient room for a bounded response."
    if "params" in value and not isinstance(value["params"], (dict, list)):
        return "JSON-RPC params must be an object or array."
    if "result" in value or "error" in value:
        return "Client messages must be requests or notifications."
    return None


def rpc_error(request_id: object, code: int, message: str,
              data: dict[str, object] | None = None) -> bytes:
    if not id_fits(request_id):
        request_id = None
    error: dict[str, object] = {"code": code, "message": message[:256]}
    if data is not None:
        error["data"] = data
    return json.dumps({"jsonrpc": "2.0", "id": request_id, "error": error},
                      ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii") + b"\n"


class SocketTransport:
    def __init__(self, connection: socket.socket):
        self.connection = connection

    def read(self, count: int) -> bytes:
        return self.connection.recv(count)

    def write(self, data: bytes) -> None:
        self.connection.sendall(data)

    def close(self) -> None:
        self.connection.close()


class Win32PipeApi:
    """Minimal byte-mode client bindings; constructed only on Windows."""

    def __init__(self):
        from ctypes import wintypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
        self.kernel.WaitNamedPipeW.restype = wintypes.BOOL
        self.kernel.ReadFile.argtypes = [
            wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        self.kernel.ReadFile.restype = wintypes.BOOL
        self.kernel.WriteFile.argtypes = list(self.kernel.ReadFile.argtypes)
        self.kernel.WriteFile.restype = wintypes.BOOL
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.dword = wintypes.DWORD

    def create_file(self, path: str) -> int:
        handle = self.kernel.CreateFileW(path, 0xC0000000, 0, None, 3, 0, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def wait(self, path: str, milliseconds: int) -> None:
        if not self.kernel.WaitNamedPipeW(path, milliseconds):
            raise ctypes.WinError(ctypes.get_last_error())

    def read(self, handle: int, count: int) -> bytes:
        buffer = ctypes.create_string_buffer(count)
        received = self.dword()
        if not self.kernel.ReadFile(handle, buffer, count, ctypes.byref(received), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return buffer.raw[:received.value]

    def write(self, handle: int, data: bytes) -> int:
        buffer = ctypes.create_string_buffer(data)
        written = self.dword()
        if not self.kernel.WriteFile(handle, buffer, len(data), ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return written.value

    def close(self, handle: int) -> None:
        self.kernel.CloseHandle(handle)


class WindowsPipeTransport:
    def __init__(self, api: Win32PipeApi, handle: int):
        self.api, self.handle = api, handle

    def read(self, count: int) -> bytes:
        return self.api.read(self.handle, count)

    def write(self, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            count = self.api.write(self.handle, data[offset:])
            if count <= 0 or count > len(data) - offset:
                raise OSError("Named pipe write made invalid progress.")
            offset += count

    def close(self) -> None:
        self.api.close(self.handle)


def connect_windows(name: str, timeout: float,
                    api: Win32PipeApi | None = None) -> Transport:
    if re.fullmatch(r"[A-Za-z0-9_.-]{1,120}", name) is None:
        raise ValueError("Windows --socket must be a local name (letters, digits, _, ., -).")
    api = api or Win32PipeApi()
    path = "\\\\.\\pipe\\" + name
    # Integer ticks avoid float cancellation at large system uptimes. A short
    # timeout must not acquire an extra millisecond when converted for Win32.
    deadline_ns = time.monotonic_ns() + math.ceil(timeout * 1_000_000_000)
    while True:
        if time.monotonic_ns() >= deadline_ns:
            raise TimeoutError("Named pipe connection deadline expired.")
        try:
            return WindowsPipeTransport(api, api.create_file(path))
        except OSError as error:
            if getattr(error, "winerror", None) != ERROR_PIPE_BUSY:
                raise
        remaining_ns = deadline_ns - time.monotonic_ns()
        if remaining_ns <= 0:
            raise TimeoutError("Named pipe connection deadline expired.")
        # WaitNamedPipe is bounded and may race another client; retry CreateFile
        # only while establishing this one connection, never after a request.
        api.wait(path, max(1, (remaining_ns + 999_999) // 1_000_000))


def connect_local(endpoint: str, timeout: float) -> Transport:
    if os.name == "nt":
        return connect_windows(endpoint, timeout)
    if not os.path.isabs(endpoint) or "\0" in endpoint:
        raise ValueError("Unix --socket must be an absolute AF_UNIX path.")
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        connection.settimeout(timeout)
        connection.connect(endpoint)
        if sys.platform.startswith("linux"):
            # Filesystem permissions protect the server; also authenticate the
            # server we connected to before forwarding any client request.
            credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                struct.calcsize("3i"))
            _, uid, _ = struct.unpack("3i", credentials)
            if uid != os.getuid():
                raise PermissionError("Kdenlive socket belongs to a different user.")
        connection.settimeout(None)
        return SocketTransport(connection)
    except BaseException:
        connection.close()
        raise


class NativeFrames:
    def __init__(self, transport: Transport):
        self.transport = transport
        self.pending = bytearray()

    def read(self) -> bytes:
        while True:
            newline = self.pending.find(b"\n")
            if newline >= 0:
                if newline > MAX_FRAME_BYTES:
                    raise ProtocolFailure("Native response exceeds the 1 MiB frame limit.")
                frame = bytes(self.pending[:newline])
                del self.pending[:newline + 1]
                return frame
            if len(self.pending) > MAX_FRAME_BYTES:
                raise ProtocolFailure("Native response exceeds the 1 MiB frame limit.")
            chunk = self.transport.read(min(READ_CHUNK_BYTES,
                                           MAX_FRAME_BYTES + 1 - len(self.pending)))
            if not chunk:
                raise EOFError("Kdenlive disconnected before its response.")
            self.pending.extend(chunk)


def native_message(frame: bytes, request_id: object) -> bool:
    """Validate output; return True for the matching response, False for notices."""
    try:
        value = load_json(frame)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ProtocolFailure("Kdenlive returned malformed JSON.") from error
    if not isinstance(value, dict) or value.get("jsonrpc") != "2.0":
        raise ProtocolFailure("Kdenlive returned an invalid JSON-RPC object.")
    if "method" in value:
        if (isinstance(value["method"], str) and "id" not in value
                and "result" not in value and "error" not in value
                and ("params" not in value or isinstance(value["params"], (dict, list)))):
            return False
        raise ProtocolFailure("Unexpected native request or malformed notification.")
    if ("id" not in value or not valid_id(value["id"])
            or type(value["id"]) is bool
            or value["id"] != request_id
            or (("result" in value) == ("error" in value))):
        raise ProtocolFailure("Kdenlive returned a mismatched or malformed response.")
    if "error" in value:
        error = value["error"]
        if (not isinstance(error, dict) or type(error.get("code")) is not int
                or not isinstance(error.get("message"), str)):
            raise ProtocolFailure("Kdenlive returned a malformed JSON-RPC error.")
    return True


class NativeChannel:
    """One connection and one transaction at a time; no automatic replay."""

    def __init__(self, connector: Callable[[float], Transport]):
        self.connector = connector
        self.commands: queue.Queue = queue.Queue(maxsize=1)
        self.events: queue.Queue = queue.Queue(maxsize=2)
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._worker, daemon=True,
                                       name="kdenlive-mcp-local-io")
        self.thread.start()

    def _emit(self, event: tuple) -> None:
        while not self.stopping.is_set():
            try:
                self.events.put(event, timeout=0.05)
                return
            except queue.Full:
                pass

    def _worker(self) -> None:
        transport = None
        frames = None
        try:
            while not self.stopping.is_set():
                command = self.commands.get()
                if command is None:
                    return
                frame, has_id, request_id, deadline = command
                uncertain = False
                try:
                    if transport is None:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise TimeoutError("Connection deadline expired.")
                        transport = self.connector(remaining)
                        frames = NativeFrames(transport)
                    if self.stopping.is_set() or time.monotonic() >= deadline:
                        raise TimeoutError("Request deadline expired before sending.")
                    # A failed send may have sent a prefix; classify conservatively.
                    uncertain = True
                    transport.write(frame + b"\n")
                    if has_id:
                        while True:
                            response = frames.read()
                            done = native_message(response, request_id)
                            self._emit(("frame", response, done))
                            if done:
                                break
                    else:
                        self._emit(("done",))
                except Exception as error:
                    self._emit(("failure", str(error)[:512], uncertain))
                    return
        finally:
            if transport is not None:
                transport.close()

    def exchange(self, frame: bytes, has_id: bool, request_id: object,
                 timeout: float, output: BinaryIO) -> None:
        deadline = time.monotonic() + timeout
        self.commands.put_nowait((frame, has_id, request_id, deadline))
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BridgeFailure("Local Kdenlive request deadline expired.", True)
            try:
                event = self.events.get(timeout=remaining)
            except queue.Empty as error:
                raise BridgeFailure("Local Kdenlive request deadline expired.", True) from error
            if event[0] == "frame":
                output.write(event[1] + b"\n")
                output.flush()
                if event[2]:
                    return
            elif event[0] == "done":
                return
            else:
                raise BridgeFailure(event[1], event[2])

    def close(self) -> None:
        self.stopping.set()
        try:
            self.commands.put_nowait(None)
        except queue.Full:
            pass
        # Never wait for a blocked OS read after an expired request.
        self.thread.join(timeout=0.05)


def run_proxy(input_stream: BinaryIO, output: BinaryIO, diagnostics: TextIO,
              connector: Callable[[float], Transport], timeout: float) -> int:
    channel = None
    try:
        while True:
            raw = input_stream.readline(MAX_FRAME_BYTES + 2)
            if not raw:
                return 0
            frame = raw[:-1] if raw.endswith(b"\n") else raw
            if len(frame) > MAX_FRAME_BYTES:
                output.write(rpc_error(None, -32700, "Input exceeds the 1 MiB frame limit."))
                output.flush()
                return 1
            if not raw.endswith(b"\n"):
                output.write(rpc_error(None, -32700, "Incomplete input frame at EOF."))
                output.flush()
                return 1
            try:
                value = load_json(frame)
            except (ValueError, UnicodeError, RecursionError):
                output.write(rpc_error(None, -32700, "Parse error."))
                output.flush()
                continue
            error = request_error(value)
            if error:
                # A structurally identifiable notification never gets a response.
                notification = (isinstance(value, dict) and "id" not in value
                                and "method" in value)
                if not notification:
                    request_id = value.get("id") if isinstance(value, dict) else None
                    output.write(rpc_error(request_id if valid_id(request_id) else None,
                                           -32600, error))
                    output.flush()
                continue
            has_id = "id" in value
            request_id = value.get("id")
            if channel is None:
                channel = NativeChannel(connector)
            try:
                channel.exchange(frame, has_id, request_id, timeout, output)
            except BridgeFailure as error:
                if has_id:
                    output.write(rpc_error(request_id, -32001, str(error), {
                        "outcome": "uncertain" if error.uncertain else "not_sent",
                        "retrySafe": not error.uncertain,
                        "proxyTerminating": True,
                    }))
                    output.flush()
                diagnostics.write("Kdenlive MCP bridge stopped: " + str(error) + "\n")
                diagnostics.flush()
                return 1
    except (BrokenPipeError, OSError) as error:
        diagnostics.write("Kdenlive MCP stdio closed: " + str(error) + "\n")
        return 1
    finally:
        if channel is not None:
            channel.close()


def timeout_value(value: str) -> float:
    result = float(value)
    if not math.isfinite(result) or not 0 < result <= 120:
        raise argparse.ArgumentTypeError("--timeout must be greater than 0 and at most 120 seconds.")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="Local Qt socket name on Windows; absolute AF_UNIX path on Unix.")
    parser.add_argument("--timeout", type=timeout_value, default=30.0,
                        help="Whole-request deadline in seconds, 0 < timeout <= 120 (default: 30).")
    args = parser.parse_args()
    if os.name == "nt":
        import msvcrt
        msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
        msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
        if re.fullmatch(r"[A-Za-z0-9_.-]{1,120}", args.socket) is None:
            parser.error("--socket must be a local name (letters, digits, _, ., -).")
    elif not os.path.isabs(args.socket) or "\0" in args.socket:
        parser.error("--socket must be an absolute AF_UNIX path.")
    return run_proxy(sys.stdin.buffer, sys.stdout.buffer, sys.stderr,
                     lambda timeout: connect_local(args.socket, timeout), args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
