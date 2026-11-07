# SPDX-FileCopyrightText: 2026 Kdenlive MCP contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Linux setup contracts; editor/socket integration remains a separate check."""

import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / "dev-tools/mcp-linux.py"
SPEC = importlib.util.spec_from_file_location("mcp_linux", PATH)
linux = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linux)
TEST_SOURCE = {"revision": "tested", "contentSha256": "a" * 64}


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux setup helper")
class LinuxSetupTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="kd-mcp-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.args = argparse.Namespace(root=self.root, socket_name="kdenlive-mcp-test", isolated=True, project=None, jobs=2)

    def installation(self):
        executable = self.root / "install/bin/kdenlive"
        executable.parent.mkdir(parents=True)
        executable.touch()
        (self.root / "build-manifest.json").write_text(json.dumps({"source": TEST_SOURCE, "nativeTestsPassed": True}))
        return executable

    def test_client_configuration_and_editor_use_identical_endpoint(self):
        executable = self.installation()
        with mock.patch.object(linux, "fingerprint", return_value=TEST_SOURCE), \
                mock.patch.object(linux, "check_runtime"), \
                mock.patch.object(linux.os, "execve") as execute, \
                mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            linux.config(self.args)
            config = json.loads(output.getvalue())["mcpServers"]["kdenlive"]
            linux.launch(self.args)
        actual_exe, argv, env = execute.call_args.args
        self.assertEqual(actual_exe, executable)
        self.assertEqual(argv, [str(executable), "--no-welcome", "--mcp-socket", self.args.socket_name])
        client_path = config["args"][config["args"].index("--socket") + 1]
        self.assertEqual(Path(client_path), Path(env["TMPDIR"]) / self.args.socket_name)
        self.assertEqual(os.stat(env["TMPDIR"]).st_mode & 0o777, 0o700)
        for key in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"):
            self.assertTrue(Path(env[key]).is_relative_to(self.root))

    def test_stale_or_unverified_build_never_launches(self):
        self.installation()
        for source in ({"revision": "another", "contentSha256": "b" * 64}, None):
            if source is None:
                (self.root / "build-manifest.json").unlink()
            with mock.patch.object(linux, "fingerprint", return_value=source), \
                    mock.patch.object(linux.os, "execve") as execute:
                with self.assertRaises(linux.SetupError):
                    linux.launch(self.args)
                execute.assert_not_called()

    def test_committing_unchanged_tested_content_does_not_block_launch(self):
        self.installation()
        source = {**TEST_SOURCE, "revision": "published-commit"}
        with mock.patch.object(linux, "fingerprint", return_value=source), \
                mock.patch.object(linux, "check_runtime"), \
                mock.patch.object(linux.os, "execve") as execute:
            linux.launch(self.args)
        execute.assert_called_once()

    @unittest.skipUnless(shutil.which("git"), "Requires Git")
    def test_source_identity_survives_staging_commit_and_tracks_real_changes(self):
        source = self.root / "source"
        source.mkdir()
        def git(*args):
            return subprocess.check_output(["git", "-C", str(source), *args], stderr=subprocess.DEVNULL)
        git("-c", "init.templateDir=", "init", "-q")
        git("config", "core.autocrlf", "false")
        (source / ".gitignore").write_text("build/\n")
        entry = source / "entry.cpp"
        entry.write_text("original\n")
        def commit():
            git("add", "--all")
            git("-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                "-c", "user.name=MCP test", "-c", "user.email=mcp@example.invalid", "commit", "-qm", "fixture")
        commit()
        with mock.patch.object(linux, "SOURCE", source):
            entry.write_text("changed\n")
            added = source / "added.py"
            added.write_text("# new build input\n")
            pending = linux.fingerprint()
            commit()
            committed = linux.fingerprint()
            self.assertNotEqual(pending["revision"], committed["revision"])
            self.assertTrue(linux.same_source(pending, committed))
            (source / "build").mkdir()
            (source / "build/output").write_text("ignored generated artifact")
            self.assertTrue(linux.same_source(committed, linux.fingerprint()))
            (source / "dev-docs").mkdir()
            (source / "dev-docs/progress.md").write_text("updated verification evidence\n")
            self.assertTrue(linux.same_source(committed, linux.fingerprint()))
            entry.write_text("another real source change\n")
            self.assertFalse(linux.same_source(committed, linux.fingerprint()))
            entry.unlink()
            deleted = linux.fingerprint()
            commit()
            self.assertTrue(linux.same_source(deleted, linux.fingerprint()))

    @unittest.skipUnless(shutil.which("git"), "Requires Git")
    def test_source_fingerprint_hashes_link_identity_without_reading_its_target(self):
        source = self.root / "source"
        source.mkdir()
        subprocess.run(["git", "-c", "init.templateDir=", "init", "-q", str(source)], check=True)
        target = self.root / "outside.txt"
        target.write_text("not a build input\n")
        link = source / "link.txt"
        link.symlink_to(target)
        subprocess.run(["git", "-C", str(source), "add", "link.txt"], check=True)
        subprocess.run(["git", "-C", str(source), "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                        "-c", "user.name=MCP test", "-c", "user.email=mcp@example.invalid", "commit", "-qm", "fixture"], check=True)
        with mock.patch.object(linux, "SOURCE", source):
            before = linux.fingerprint()
            target.write_text("changed outside content\n")
            self.assertTrue(linux.same_source(before, linux.fingerprint()))
            link.unlink()
            link.symlink_to(self.root / "another-target.txt")
            self.assertFalse(linux.same_source(before, linux.fingerprint()))

    @unittest.skipUnless(shutil.which("pkg-config"), "Requires pkg-config")
    def test_build_queries_the_matching_mlt_pkg_config_prefix(self):
        # src/CMakeLists.txt obtains MLT_DATADIR and MLT_MELTBIN through
        # pkg-config separately from find_package(Mlt7). Both must resolve to
        # our patched dependency rather than the distribution's older MLT.
        prefix = self.root / "install"
        configs = prefix / "lib/pkgconfig"
        configs.mkdir(parents=True)
        (configs / "mlt-framework-7.pc").write_text(
            f"prefix={prefix}\nmeltbin=${{prefix}}/bin/melt\nmltdatadir=${{prefix}}/share/mlt-7\n"
            "Name: patched MLT fixture\nDescription: isolated dependency\nVersion: 7.43.0\n")
        env = linux.runtime_env(self.root)
        for variable, expected in (("meltbin", prefix / "bin/melt"), ("mltdatadir", prefix / "share/mlt-7")):
            value = subprocess.check_output(["pkg-config", f"--variable={variable}", "mlt-framework-7"], env=env, text=True)
            self.assertEqual(value.strip(), str(expected))

    def test_existing_endpoint_is_never_removed(self):
        self.installation()
        path = linux.endpoint(self.args)
        path.parent.mkdir(mode=0o700)
        path.write_text("existing endpoint")
        with mock.patch.object(linux, "fingerprint", return_value=TEST_SOURCE), \
                mock.patch.object(linux, "check_runtime"), \
                mock.patch.object(linux.os, "execve") as execute:
            with self.assertRaises(linux.SetupError):
                linux.launch(self.args)
        self.assertEqual(path.read_text(), "existing endpoint")
        execute.assert_not_called()

    def test_shared_or_symlinked_runtime_directory_is_refused(self):
        self.installation()
        directory = self.root / "run"
        target = self.root / "other"
        target.mkdir(mode=0o700)
        for symlink in (False, True):
            with self.subTest(symlink=symlink):
                if symlink:
                    directory.symlink_to(target, target_is_directory=True)
                else:
                    directory.mkdir(mode=0o755)
                    directory.chmod(0o755)
                with mock.patch.object(linux, "fingerprint", return_value=TEST_SOURCE), \
                        mock.patch.object(linux, "check_runtime"), \
                        mock.patch.object(linux.os, "execve") as execute:
                    with self.assertRaises(linux.SetupError):
                        linux.launch(self.args)
                    execute.assert_not_called()
                if symlink:
                    directory.unlink()
                else:
                    directory.rmdir()

    def test_missing_build_tools_stop_before_download_or_build(self):
        with mock.patch.object(linux.shutil, "which", return_value=None), \
                mock.patch.object(linux, "prepare_mlt") as prepare, \
                mock.patch.object(linux, "run") as run:
            with self.assertRaises(linux.SetupError):
                linux.build(self.args)
        prepare.assert_not_called()
        run.assert_not_called()

    @unittest.skipUnless(shutil.which("git"), "Requires Git")
    def test_crlf_dependency_patch_applies_and_repeat_does_not_replay_it(self):
        source = self.root / "mlt-source"
        source.mkdir()
        def git(*args):
            return subprocess.check_output(["git", "-C", str(source), *args], stderr=subprocess.DEVNULL).decode().strip()
        git("init", "-q")
        git("config", "core.autocrlf", "false")
        (source / "entry.txt").write_bytes(b"original\n")
        git("add", "entry.txt")
        git("-c", "user.name=MCP test", "-c", "user.email=mcp@example.invalid", "commit", "-qm", "fixture")
        revision = git("rev-parse", "HEAD")
        patches = self.root / "fixture/packaging/patches"
        patches.mkdir(parents=True)
        (patches / "fix.patch").write_bytes(
            b"diff --git a/entry.txt b/entry.txt\r\n--- a/entry.txt\r\n+++ b/entry.txt\r\n@@ -1 +1 @@\r\n-original\r\n+patched\r\n")
        with mock.patch.object(linux, "SOURCE", self.root / "fixture"), \
                mock.patch.object(linux, "MLT_REVISION", revision), \
                mock.patch.object(linux, "PATCHES", ("fix.patch",)):
            self.assertEqual(linux.prepare_mlt(self.root), source)
            self.assertEqual((source / "entry.txt").read_bytes(), b"patched\n")
            first_diff = git("diff")
            linux.prepare_mlt(self.root)
            self.assertEqual(git("diff"), first_diff)
        with mock.patch.object(linux, "MLT_REVISION", "different-revision"):
            with self.assertRaises(linux.SetupError):
                linux.prepare_mlt(self.root)
        self.assertEqual((source / "entry.txt").read_bytes(), b"patched\n")

    def test_socket_names_and_linux_path_limit_are_validated(self):
        for name in ("short", "../kdenlive", "/tmp/kdenlive", "kdenlive\0bad", "é" * 10):
            with self.subTest(name=name):
                self.args.socket_name = name
                with self.assertRaises(linux.SetupError):
                    linux.endpoint(self.args)
        self.args.socket_name = "kdenlive-mcp-test"
        self.args.root = Path("/tmp") / ("x" * 100)
        with self.assertRaises(linux.SetupError):
            linux.endpoint(self.args)

    def test_read_only_probe_handshake_and_catalog_do_not_claim_full_coverage(self):
        bridge = mock.Mock()
        loader = mock.Mock()
        loader.exec_module.side_effect = lambda module: None
        bridge.run_proxy.side_effect = lambda inputs, output, diagnostics, connector, timeout: self.probe_reply(inputs, output)
        with mock.patch.object(linux.importlib.util, "spec_from_file_location", return_value=argparse.Namespace(loader=loader)), \
                mock.patch.object(linux.importlib.util, "module_from_spec", return_value=bridge), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(linux.probe(self.args), 0)
        report = json.loads(output.getvalue())
        self.assertTrue(report["connected"])
        self.assertFalse(report["editsPerformed"])
        self.assertFalse(report["fullEditorCoverageVerified"])

    def probe_reply(self, inputs, output):
        requests = list(map(json.loads, inputs.getvalue().splitlines()))
        self.assertEqual(requests[0]["method"], "initialize")
        self.assertNotIn("id", requests[1])
        self.assertEqual([request["params"]["name"] for request in requests[3:]],
                         ["get_info", "get_capabilities", "get_state"])
        tools = [{"name": name} for name in ("get_info", "get_capabilities", "get_state")]
        results = {1: {}, 2: {"tools": tools}, 3: {"structuredContent": {"ok": True, "application": "Kdenlive"}},
                   4: {"structuredContent": {"ok": True}}, 5: {"structuredContent": {"ok": True}}}
        for index, result in results.items():
            output.write(json.dumps({"jsonrpc": "2.0", "id": index, "result": result}).encode() + b"\n")
        return 0


if __name__ == "__main__":
    unittest.main()
