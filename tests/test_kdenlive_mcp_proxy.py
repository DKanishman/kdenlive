# SPDX-FileCopyrightText: 2026 Kdenlive MCP prototype contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable regression tests for the standalone local MCP bridge."""

import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest import mock
import uuid

PROXY_PATH = Path(__file__).resolve().parents[1] / "src" / "mcp" / "kdenlive-mcp.py"
if not PROXY_PATH.is_file():
    PROXY_PATH = Path(__file__).with_name("kdenlive-mcp.py")  # Task staging.
SPEC = importlib.util.spec_from_file_location("kdenlive_mcp_proxy", PROXY_PATH)
proxy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(proxy)


def wire(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"


def request(request_id=1, method="ping"):
    return {"jsonrpc": "2.0", "id": request_id, "method": method}


def response(request_id=1, result=None):
    return {"jsonrpc": "2.0", "id": request_id,
            "result": {} if result is None else result}


class MemoryTransport:
    def __init__(self, replies=b"", chunk=7):
        self.replies = io.BytesIO(replies)
        self.chunk = chunk
        self.writes = []
        self.closed = threading.Event()

    def read(self, count):
        return self.replies.read(min(count, self.chunk))

    def write(self, data):
        self.writes.append(data)

    def close(self):
        self.closed.set()


class ProxyTests(unittest.TestCase):
    def run_bridge(self, inputs, transport, timeout=0.5, connector=None):
        output, diagnostics = io.BytesIO(), io.StringIO()
        calls = []
        def connect(remaining):
            calls.append(remaining)
            return transport
        exit_code = proxy.run_proxy(
            io.BytesIO(inputs), output, diagnostics,
            connector or connect, timeout)
        messages = [json.loads(line) for line in output.getvalue().splitlines()]
        return exit_code, messages, diagnostics.getvalue(), calls

    def test_preserves_ids_unicode_and_exact_native_payload(self):
        ids = ["request-שלום", 0, -7, None, 9007199254740993]
        native = b"".join(wire(response(value, {"structuredContent": {"ok": True}}))
                          for value in ids)
        transport = MemoryTransport(native)
        inputs = b"".join(wire(request(value)) for value in ids)
        code, messages, diagnostics, calls = self.run_bridge(inputs, transport)
        self.assertEqual(code, 0)
        self.assertEqual([m["id"] for m in messages], ids)
        self.assertEqual(transport.writes, inputs.splitlines(keepends=True))
        self.assertEqual(len(calls), 1)
        self.assertEqual(diagnostics, "")
        self.assertTrue(transport.closed.wait(0.5))

    def test_notifications_have_no_response_and_preserve_sequence(self):
        notification = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        transport = MemoryTransport(wire(response("next")))
        code, messages, _, _ = self.run_bridge(
            wire(notification) + wire(request("next")), transport)
        self.assertEqual(code, 0)
        self.assertEqual(messages, [response("next")])
        self.assertEqual(transport.writes,
                         [wire(notification), wire(request("next"))])

    def test_native_notifications_forward_before_matching_response(self):
        notice = {"jsonrpc": "2.0", "method": "notifications/message",
                  "params": {"data": "working"}}
        transport = MemoryTransport(wire(notice) + wire(response()))
        code, messages, _, _ = self.run_bridge(wire(request()), transport)
        self.assertEqual(code, 0)
        self.assertEqual(messages, [notice, response()])

    def test_parse_errors_and_invalid_requests_are_never_forwarded(self):
        invalid = [b"{broken\n", b"\xff\n", b'{"jsonrpc":"2.0","method":"ping","id":NaN}\n',
                   wire([]), wire(request(True))]
        transport = MemoryTransport(wire(response("valid")))
        code, messages, _, calls = self.run_bridge(
            b"".join(invalid) + wire(request("valid")), transport)
        self.assertEqual(code, 0)
        self.assertEqual([m["error"]["code"] for m in messages[:-1]],
                         [-32700, -32700, -32700, -32600, -32600])
        self.assertEqual(transport.writes, [wire(request("valid"))])
        self.assertEqual(len(calls), 1)

    def test_excessively_nested_json_is_rejected_locally(self):
        nested = b"[" * 20000 + b"0" + b"]" * 20000 + b"\n"
        transport = MemoryTransport(wire(response("valid")))
        code, messages, _, calls = self.run_bridge(nested + wire(request("valid")), transport)
        self.assertEqual(code, 0)
        self.assertEqual(messages[0]["error"]["code"], -32700)
        self.assertEqual(transport.writes, [wire(request("valid"))])
        self.assertEqual(len(calls), 1)

    def test_malformed_notification_is_silent_and_never_connects(self):
        invalid = [
            {"jsonrpc": "2.0", "method": "notifications/initialized", "params": 3},
            {"jsonrpc": "2.0", "method": 42},
            {"jsonrpc": "1.0", "method": "notifications/initialized"},
        ]
        for value in invalid:
            with self.subTest(value=value):
                transport = MemoryTransport()
                code, messages, _, calls = self.run_bridge(wire(value), transport)
                self.assertEqual((code, messages, calls, transport.writes), (0, [], [], []))

    def test_input_size_and_unterminated_frame_fail_before_connect(self):
        for data in (b"x" * (proxy.MAX_FRAME_BYTES + 1) + b"\n",
                     wire(request()).rstrip(b"\n")):
            with self.subTest(length=len(data)):
                transport = MemoryTransport()
                code, messages, _, calls = self.run_bridge(data, transport)
                self.assertEqual(code, 1)
                self.assertEqual(messages[0]["error"]["code"], -32700)
                self.assertEqual(calls, [])
                self.assertEqual(transport.writes, [])

    def test_malformed_oversize_or_mismatched_native_reply_terminates_without_replay(self):
        replies = [b"{bad\n", wire(response("wrong")),
                   wire(response(True)), b"x" * (proxy.MAX_FRAME_BYTES + 1) + b"\n",
                   b"", wire({"jsonrpc": "2.0", "id": 1, "error": "invalid"})]
        for native in replies:
            with self.subTest(native_prefix=native[:30]):
                transport = MemoryTransport(native, chunk=65536)
                code, messages, diagnostics, calls = self.run_bridge(
                    wire(request()) + wire(request(2)), transport)
                self.assertEqual(code, 1)
                self.assertEqual(len(messages), 1)
                self.assertEqual(messages[0]["id"], 1)
                self.assertEqual(messages[0]["error"]["data"]["outcome"], "uncertain")
                self.assertFalse(messages[0]["error"]["data"]["retrySafe"])
                self.assertTrue(messages[0]["error"]["data"]["proxyTerminating"])
                self.assertEqual(transport.writes, [wire(request())])
                self.assertEqual(len(calls), 1)
                self.assertIn("stopped", diagnostics)

    def test_native_json_rpc_error_is_relayed_unchanged(self):
        error = {"jsonrpc": "2.0", "id": "missing",
                 "error": {"code": -32601, "message": "Method not found"}}
        transport = MemoryTransport(wire(error))
        code, messages, _, _ = self.run_bridge(
            wire(request("missing", "unsupported")), transport)
        self.assertEqual(code, 0)
        self.assertEqual(messages, [error])

    def test_connect_failure_is_not_sent_and_no_reconnect(self):
        calls = []
        def unavailable(_):
            calls.append(1)
            raise FileNotFoundError("Local socket unavailable")
        transport = MemoryTransport()
        code, messages, _, _ = self.run_bridge(
            wire(request()) + wire(request(2)), transport, connector=unavailable)
        self.assertEqual(code, 1)
        self.assertEqual(calls, [1])
        self.assertEqual(messages[0]["error"]["data"]["outcome"], "not_sent")
        self.assertTrue(messages[0]["error"]["data"]["retrySafe"])

    def test_send_failure_is_uncertain_and_never_replayed(self):
        class FailingWrite(MemoryTransport):
            def write(self, data):
                self.writes.append(data[:3])
                raise BrokenPipeError("Partial write")
        transport = FailingWrite()
        code, messages, _, calls = self.run_bridge(
            wire(request()) + wire(request(2)), transport)
        self.assertEqual(code, 1)
        self.assertEqual(messages[0]["error"]["data"]["outcome"], "uncertain")
        self.assertEqual(len(transport.writes), 1)
        self.assertEqual(len(calls), 1)

    def test_response_timeout_is_bounded_and_never_replays(self):
        release = threading.Event()
        class BlockedRead(MemoryTransport):
            def read(self, count):
                release.wait(2)
                return b""
        transport = BlockedRead()
        start = time.monotonic()
        try:
            code, messages, _, calls = self.run_bridge(
                wire(request()) + wire(request(2)), transport, timeout=0.03)
            self.assertLess(time.monotonic() - start, 0.5)
            self.assertEqual(code, 1)
            self.assertEqual(len(messages), 1)
            self.assertEqual(messages[0]["error"]["data"]["outcome"], "uncertain")
            self.assertEqual(len(calls), 1)
            self.assertEqual(transport.writes, [wire(request())])
        finally:
            release.set()
        self.assertTrue(transport.closed.wait(0.5))

    def test_late_connect_cannot_send_after_deadline(self):
        release = threading.Event()
        transport = MemoryTransport()
        def delayed_connect(_):
            release.wait(2)
            return transport
        try:
            code, _, _, _ = self.run_bridge(
                wire(request()), transport, timeout=0.03, connector=delayed_connect)
            self.assertEqual(code, 1)
        finally:
            release.set()
        self.assertTrue(transport.closed.wait(0.5))
        self.assertEqual(transport.writes, [])

    def test_notification_transport_failure_has_no_json_rpc_response(self):
        notification = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        def unavailable(_):
            raise FileNotFoundError("No local socket")
        code, messages, diagnostics, _ = self.run_bridge(
            wire(notification), MemoryTransport(), connector=unavailable)
        self.assertEqual(code, 1)
        self.assertEqual(messages, [])
        self.assertIn("stopped", diagnostics)

    def test_exact_frame_limit_and_fragmented_newline(self):
        with mock.patch.object(proxy, "MAX_FRAME_BYTES", 8):
            frames = proxy.NativeFrames(MemoryTransport(b"12345678\nnext\n", chunk=2))
            self.assertEqual(frames.read(), b"12345678")
            self.assertEqual(frames.read(), b"next")
            with self.assertRaises(EOFError):
                frames.read()
            frames = proxy.NativeFrames(MemoryTransport(b"123456789\n", chunk=2))
            with self.assertRaises(proxy.ProtocolFailure):
                frames.read()

    def test_large_unicode_id_is_rejected_before_forwarding_and_errors_are_bounded(self):
        large_id = "א" * (proxy.MAX_ID_BYTES // 6 + 1)
        transport = MemoryTransport(wire(response("valid")))
        code, messages, _, calls = self.run_bridge(
            wire(request(large_id)) + wire(request("valid")), transport)
        self.assertEqual(code, 0)
        self.assertEqual(messages[0]["id"], None)
        self.assertEqual(messages[0]["error"]["code"], -32600)
        self.assertEqual(transport.writes, [wire(request("valid"))])
        self.assertEqual(len(calls), 1)
        self.assertLessEqual(len(proxy.rpc_error(large_id, -32001, "א" * 100000)),
                             proxy.MAX_FRAME_BYTES + 1)

    def test_timeout_argument_bounds(self):
        for value in ("0", "-1", "121", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(proxy.argparse.ArgumentTypeError):
                proxy.timeout_value(value)
        self.assertEqual(proxy.timeout_value("120"), 120)
        self.assertEqual(proxy.timeout_value("0.1"), 0.1)


class FakePipeApi:
    def __init__(self, opens=None):
        self.opens = list(opens or [77])
        self.paths = []
        self.waits = []
        self.writes = []
        self.closed = []

    def create_file(self, path):
        self.paths.append(path)
        value = self.opens.pop(0)
        if isinstance(value, BaseException):
            raise value
        return value

    def wait(self, path, milliseconds):
        self.waits.append((path, milliseconds))

    def write(self, handle, data):
        self.writes.append((handle, data[:2]))
        return min(2, len(data))

    def read(self, handle, count):
        return b"ok"[:count]

    def close(self, handle):
        self.closed.append(handle)


class WindowsPipeTests(unittest.TestCase):
    @staticmethod
    def busy():
        error = OSError("Pipe busy")
        error.winerror = proxy.ERROR_PIPE_BUSY
        return error

    def test_busy_race_waits_and_preserves_local_only_path(self):
        api = FakePipeApi([self.busy(), self.busy(), 77])
        transport = proxy.connect_windows("kdenlive-local-test", 0.2, api=api)
        self.assertEqual(api.paths, [r"\\.\pipe\kdenlive-local-test"] * 3)
        self.assertEqual(len(api.waits), 2)
        self.assertTrue(all(0 < timeout <= 200 for _, timeout in api.waits))
        transport.write(b"abcde")
        self.assertEqual(b"".join(data for _, data in api.writes), b"abcde")
        self.assertEqual(transport.read(2), b"ok")
        transport.close()
        self.assertEqual(api.closed, [77])

    def test_busy_wait_budget_uses_exact_ticks_at_large_uptime(self):
        api = FakePipeApi([self.busy(), self.busy(), 77])
        initial = 1_000_000_000_000_000_000
        ticks = [initial, initial, initial, initial + 100_000_000,
                 initial + 100_000_000, initial + 199_000_000]
        with mock.patch.object(proxy.time, "monotonic_ns", side_effect=ticks):
            transport = proxy.connect_windows("kdenlive-local-test", 0.2, api=api)
        self.assertEqual([milliseconds for _, milliseconds in api.waits], [200, 100])
        transport.close()
        self.assertEqual(api.closed, [77])

    def test_busy_deadline_is_rechecked_after_wait(self):
        api = FakePipeApi([self.busy()])
        ticks = [0, 0, 199_999_999, 200_000_000]
        with mock.patch.object(proxy.time, "monotonic_ns", side_effect=ticks):
            with self.assertRaises(TimeoutError):
                proxy.connect_windows("kdenlive-local-test", 0.2, api=api)
        self.assertEqual(len(api.paths), 1)
        self.assertEqual([milliseconds for _, milliseconds in api.waits], [1])

    def test_nonbusy_error_is_not_retried(self):
        api = FakePipeApi([FileNotFoundError("Absent")])
        with self.assertRaises(FileNotFoundError):
            proxy.connect_windows("absent", 0.2, api=api)
        self.assertEqual(len(api.paths), 1)
        self.assertEqual(api.waits, [])

    def test_wait_timeout_does_not_open_again(self):
        api = FakePipeApi([self.busy()])
        api.wait = mock.Mock(side_effect=TimeoutError("Busy deadline"))
        with self.assertRaises(TimeoutError):
            proxy.connect_windows("busy", 0.2, api=api)
        self.assertEqual(len(api.paths), 1)

    def test_remote_or_path_socket_names_are_rejected(self):
        for name in (r"\\remote\pipe\name", r"\\.\pipe\name", "../name", "", "x" * 121):
            with self.subTest(name=name), self.assertRaises(ValueError):
                proxy.connect_windows(name, 0.2, api=FakePipeApi())

    @unittest.skipUnless(os.name == "nt", "Requires Windows named pipes")
    def test_real_windows_named_pipe_roundtrip(self):
        from ctypes import wintypes
        api = proxy.Win32PipeApi()
        kernel = api.kernel
        kernel.CreateNamedPipeW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, proxy.ctypes.c_void_p]
        kernel.CreateNamedPipeW.restype = wintypes.HANDLE
        kernel.ConnectNamedPipe.argtypes = [wintypes.HANDLE, proxy.ctypes.c_void_p]
        kernel.ConnectNamedPipe.restype = wintypes.BOOL
        kernel.FlushFileBuffers.argtypes = [wintypes.HANDLE]
        kernel.FlushFileBuffers.restype = wintypes.BOOL
        name = "kdenlive-mcp-test-" + uuid.uuid4().hex
        path = "\\\\.\\pipe\\" + name
        handle = kernel.CreateNamedPipeW(path, 0x00080003, 0, 1, 4096, 4096, 1000, None)
        self.assertNotEqual(handle, proxy.ctypes.c_void_p(-1).value)
        received, failures = [], []
        def server():
            try:
                if not kernel.ConnectNamedPipe(handle, None):
                    error = proxy.ctypes.get_last_error()
                    if error != 535:  # ERROR_PIPE_CONNECTED
                        raise proxy.ctypes.WinError(error)
                incoming = proxy.NativeFrames(proxy.WindowsPipeTransport(api, handle)).read()
                received.append(incoming)
                proxy.WindowsPipeTransport(api, handle).write(wire(response("pipe")))
                kernel.FlushFileBuffers(handle)
            except Exception as error:
                failures.append(error)
            finally:
                api.close(handle)
        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        output, diagnostics = io.BytesIO(), io.StringIO()
        code = proxy.run_proxy(io.BytesIO(wire(request("pipe"))), output, diagnostics,
                               lambda timeout: proxy.connect_windows(name, timeout), 1.0)
        thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(failures, [])
        self.assertEqual(code, 0, diagnostics.getvalue())
        self.assertEqual(received, [wire(request("pipe")).rstrip(b"\n")])
        self.assertEqual(json.loads(output.getvalue()), response("pipe"))


@unittest.skipUnless(os.name == "posix" and hasattr(socket, "AF_UNIX"),
                     "Requires a Unix AF_UNIX socket")
class UnixSocketTests(unittest.TestCase):
    def test_real_unix_socket_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix="kd-mcp-") as directory:
            path = os.path.join(directory, "socket")
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(path)
            listener.listen(1)
            failures, received = [], []
            def server():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        transport = proxy.SocketTransport(connection)
                        received.append(proxy.NativeFrames(transport).read())
                        transport.write(wire(response("unix")))
                except Exception as error:
                    failures.append(error)
            thread = threading.Thread(target=server, daemon=True)
            thread.start()
            output, diagnostics = io.BytesIO(), io.StringIO()
            try:
                code = proxy.run_proxy(io.BytesIO(wire(request("unix"))), output,
                                       diagnostics,
                                       lambda timeout: proxy.connect_local(path, timeout), 1)
                thread.join(1)
                self.assertFalse(thread.is_alive())
                self.assertEqual(failures, [])
                self.assertEqual(code, 0, diagnostics.getvalue())
                self.assertEqual(received, [wire(request("unix")).rstrip(b"\n")])
                self.assertEqual(json.loads(output.getvalue()), response("unix"))
            finally:
                listener.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
