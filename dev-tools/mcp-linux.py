#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive MCP contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build, diagnose and connect the native MCP fork on Linux; no root operations."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
MLT_REVISION = "eca4e32d28e2c588a9c3b7c23e2276d9cd1f4edb"
PATCHES = (
    "mlt-playlist-entry-info.patch",
    "mlt-sdl2-audio-device-lifetime.patch",
    "mlt-openfx-factory-export.patch",
    "mlt-xml-windows-getcwd.patch",
    "mlt-rotoscoping-bounds.patch",
    "mlt-metadata-validation.patch",
    "mlt-private-cjson.patch",
)
KWALIFY_VERSION = "0.7.2"
REQUIRED_TOOLS = ("git", "cmake", "ctest", "ninja", "cc", "c++", "pkg-config", "ffmpeg")
SUITES = "^(mcpalignmenttest|mcptest|subtitlestest|groupstest|sequencetest|markertest|otiotest|ffttoolstest|bintest|mcpprojectextrastest|mcp_proxy|mcp_linux)$"


class SetupError(Exception):
    pass


def run(argv, *, cwd=None, env=None, capture=False):
    argv = [str(value) for value in argv]
    print("+ " + shlex.join(argv), file=sys.stderr, flush=True)
    return subprocess.run(argv, cwd=cwd, env=env, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def fingerprint():
    """Hash effective source content independently of staging and commit state."""
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE).decode().strip()
    names = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=SOURCE)
    digest = hashlib.sha256(b"kdenlive-mcp-source-v2\0")
    for name in sorted(set(names.split(b"\0")) - {b""}):
        if name.startswith(b"dev-docs/") or name in {b"README.md", b"README.md.license"}:
            continue  # Progress reports are not application/build inputs.
        path = SOURCE / os.fsdecode(name)
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue  # A deletion has the same effective content before/after commit.
        digest.update(len(name).to_bytes(8, "big") + name)
        if stat.S_ISLNK(metadata.st_mode):
            target = os.fsencode(os.readlink(path))
            digest.update(b"link\0" + len(target).to_bytes(8, "big") + target)
        elif stat.S_ISREG(metadata.st_mode):
            digest.update(b"file\0" + bytes([bool(metadata.st_mode & 0o111)]))
            digest.update(metadata.st_size.to_bytes(8, "big"))
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
        else:
            raise SetupError("Unsupported source entry: " + str(path))
    return {"revision": revision, "contentSha256": digest.hexdigest()}


def same_source(first, second):
    """Git synchronization alone must not make an unchanged tested app stale."""
    return (isinstance(first, dict) and isinstance(second, dict)
            and isinstance(first.get("contentSha256"), str) and bool(first["contentSha256"])
            and first["contentSha256"] == second.get("contentSha256"))


def endpoint(args):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{7,99}", args.socket_name):
        raise SetupError("Socket names require 8-100 ASCII letters, digits, dots, underscores or hyphens.")
    path = args.root / "run" / args.socket_name
    if len(os.fsencode(path)) > 107:
        raise SetupError("AF_UNIX path exceeds 107 bytes; use a shorter --root or --socket-name.")
    return path


def runtime_env(root, *, isolated=False):
    prefix = root / "install"
    env = os.environ.copy()
    for key, paths in {
        "PATH": [prefix / "bin"],
        "LD_LIBRARY_PATH": [prefix / "lib", prefix / "lib64"],
        "PKG_CONFIG_PATH": [prefix / "lib/pkgconfig", prefix / "lib64/pkgconfig"],
        "XDG_DATA_DIRS": [prefix / "share"],
    }.items():
        fallback = "/usr/local/share:/usr/share" if key == "XDG_DATA_DIRS" else ""
        env[key] = ":".join(map(str, paths)) + ":" + (env.get(key) or fallback)
        env[key] = env[key].rstrip(":")
    # Optional distribution runtime packages can be unpacked into this private
    # tree when installing system-wide Qt dependencies is not available.
    qt = root / "qt-runtime/usr/lib64/qt6"
    for variable, directory in (("QML_IMPORT_PATH", qt / "qml"), ("QT_PLUGIN_PATH", qt / "plugins")):
        if directory.is_dir():
            env[variable] = str(directory) + (":" + env[variable] if env.get(variable) else "")
    env.update(MLT_REPOSITORY=str(prefix / "lib/mlt-7"),
               MLT_DATA=str(prefix / "share/mlt-7"),
               MLT_PROFILES_PATH=str(prefix / "share/mlt-7/profiles"),
               MLT_PRESETS_PATH=str(prefix / "share/mlt-7/presets"),
               MLT_PREFIX=str(prefix))
    if isolated:
        for key, name in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                          ("XDG_DATA_HOME", "data")):
            path = root / "test-state" / name
            path.mkdir(parents=True, exist_ok=True)
            env[key] = str(path)
    return env


def prepare_test_tools(root):
    """Use a system Kwalify or an isolated pinned Ruby validator for MLT tests."""
    env = os.environ.copy()
    if shutil.which("kwalify"):
        return env
    if not shutil.which("ruby") or not shutil.which("gem"):
        raise SetupError("MLT tests require Kwalify, or Ruby and gem for the isolated test utility.")
    gems = root / "test-gems"
    tools = root / "test-tools"
    env["GEM_SPEC_CACHE"] = str(root / "gem-cache")
    if not (gems / "specifications" / f"kwalify-{KWALIFY_VERSION}.gemspec").is_file():
        run(["gem", "install", "kwalify", "--version", KWALIFY_VERSION,
             "--install-dir", gems, "--bindir", tools, "--no-document"], env=env)
    tools.mkdir(exist_ok=True)
    validator = tools / "kwalify"
    # Ruby 4 removed StringScanner#peep and Psych#load_documents. Use the
    # equivalent scanner name and Kwalify's own line-aware YAML parser.
    validator.write_text("#!/usr/bin/env ruby\nrequire 'strscan'\n"
                         "class StringScanner\n  alias_method :peep, :peek unless method_defined?(:peep)\nend\n"
                         "ARGV.unshift('-l')\n"
                         f"load Gem.bin_path('kwalify', 'kwalify', '{KWALIFY_VERSION}')\n")
    validator.chmod(0o755)
    env["GEM_PATH"] = str(gems) + (":" + env["GEM_PATH"] if env.get("GEM_PATH") else "")
    env["PATH"] = str(tools) + ":" + env.get("PATH", "")
    run([validator, "-v"], env=env)
    return env


def doctor(args):
    tools = {name: shutil.which(name) for name in REQUIRED_TOOLS}
    report = {"platform": sys.platform, "python": sys.version.split()[0],
              "source": str(SOURCE), "sourceState": fingerprint(), "tools": tools,
              "requiredVersions": {"Qt": "6.10.0", "KF": "6.21.0", "MLT": "7.42.0 plus supplied patches"},
              "executable": str(args.root / "install/bin/kdenlive"),
              "nativeInstalled": (args.root / "install/bin/kdenlive").is_file(),
              "displayAvailable": bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")),
              "endpoint": str(endpoint(args)), "fullEditorCoverageVerified": False}
    try:
        with tempfile.TemporaryDirectory(prefix="kd-mcp-") as directory:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(Path(directory) / "probe"))
        report["unixSocketBind"] = {"ok": True}
    except OSError as error:
        report["unixSocketBind"] = {"ok": False, "error": str(error)}
    report["readyForBuild"] = all(tools.values()) and report["unixSocketBind"]["ok"]
    report["fedoraDependencies"] = [
        "sudo dnf install git cmake ninja-build gcc-c++ extra-cmake-modules pkgconf-pkg-config",
        "sudo dnf builddep mlt kdenlive",
    ]
    print(json.dumps(report, indent=2))
    return 0 if report["readyForBuild"] else 1


def prepare_mlt(root):
    source = root / "mlt-source"
    if not source.exists():
        run(["git", "clone", "--no-checkout", "https://github.com/mltframework/mlt.git", source])
        run(["git", "-C", source, "checkout", "--detach", MLT_REVISION])
    revision = run(["git", "-C", source, "rev-parse", "HEAD"], capture=True).strip()
    if revision != MLT_REVISION:
        raise SetupError("Existing MLT checkout has another revision; it will not be reset automatically.")
    normalized = root / "patches"
    normalized.mkdir(exist_ok=True)
    for name in PATCHES:
        patch = normalized / name
        # The recovered Windows checkout can contain CRLF patch files even
        # though MLT's Linux checkout has LF. Normalize a copy, not the source.
        patch.write_bytes((SOURCE / "packaging/patches" / name).read_bytes().replace(b"\r\n", b"\n"))
        # A repeat build is safe only when a patch applies cleanly or is already
        # present exactly. Never reset or overwrite an existing dependency tree.
        reverse = subprocess.run(["git", "-C", str(source), "apply", "--reverse", "--check", str(patch)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if reverse.returncode:
            run(["git", "-C", source, "apply", "--check", patch])
            run(["git", "-C", source, "apply", patch])
    return source


def check_runtime(root):
    prefix = root / "install"
    required = [prefix / "bin/melt", prefix / "share/mlt-7/profiles",
                *[prefix / "lib/mlt-7" / ("libmlt" + module + ".so")
                  for module in ("core", "avformat", "xml", "qt6", "sdl2", "plusgpl")]]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise SetupError("MLT build lacks required runtime components: " + ", ".join(missing))


def build(args):
    missing = [name for name in REQUIRED_TOOLS if not shutil.which(name)]
    if missing:
        raise SetupError("Missing build tools: " + ", ".join(missing) + ". Run doctor for Fedora setup commands.")
    args.root.mkdir(parents=True, exist_ok=True)
    stamp = args.root / "build-manifest.json"
    # A failed rebuild must never leave an earlier success receipt in place.
    stamp.unlink(missing_ok=True)
    before = fingerprint()
    test_tools_env = prepare_test_tools(args.root)
    mlt_source = prepare_mlt(args.root)
    prefix = args.root / "install"
    mlt_build = args.root / "mlt-build"
    editor_build = args.root / "editor-build"
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_INSTALL_LIBDIR=lib",
              "-DCMAKE_INSTALL_PREFIX=" + str(prefix), "-DBUILD_TESTING=ON"]
    run(["cmake", "-S", mlt_source, "-B", mlt_build, *common,
         "-DMOD_AVFORMAT=ON", "-DMOD_QT6=ON", "-DMOD_XML=ON", "-DMOD_SDL2=ON",
         "-DMOD_OPENFX=ON", "-DMOD_PLUSGPL=ON", "-DGPL=ON", "-DGPL3=ON"], env=test_tools_env)
    run(["cmake", "--build", mlt_build, "--parallel", args.jobs], env=test_tools_env)
    run(["cmake", "--install", mlt_build], env=test_tools_env)
    check_runtime(args.root)
    env = runtime_env(args.root, isolated=True)
    for key in ("PATH", "GEM_PATH", "GEM_SPEC_CACHE"):
        if key in test_tools_env:
            env[key] = (str(prefix / "bin") + ":" if key == "PATH" else "") + test_tools_env[key]
    env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", SDL_AUDIODRIVER="dummy")
    run(["ctest", "--test-dir", mlt_build, "--output-on-failure", "--no-tests=error", "--timeout", "900"], env=env)
    run(["cmake", "-S", SOURCE, "-B", editor_build, *common,
         "-DKDE_INSTALL_LIBDIR=lib", "-DRELEASE_BUILD=OFF", "-DBUILD_QCH=OFF",
         "-DKDE_INSTALL_USE_QT_SYS_PATHS=OFF",
         "-DCMAKE_PREFIX_PATH=" + str(prefix), "-DMlt7_DIR=" + str(prefix / "lib/cmake/Mlt7"),
         "-DPython3_EXECUTABLE=" + sys.executable], env=env)
    run(["cmake", "--build", editor_build, "--parallel", args.jobs], env=env)
    # Native tests discover effects/export presets with QStandardPaths. The
    # stock Flatpak does not supply host-side data, so stage our own data first.
    # The executable and successful-build receipt are published only afterward.
    run(["cmake", "--install", editor_build / "data"], env=env)
    run(["ctest", "--test-dir", editor_build, "-R", SUITES, "--output-on-failure",
         "--no-tests=error", "--timeout", "1800"], env=env)
    if not same_source(fingerprint(), before):
        raise SetupError("Source changed while compiling/testing. Rebuild before installing this editor.")
    run(["cmake", "--install", editor_build], env=env)
    dependency_diff = subprocess.check_output(["git", "-C", str(mlt_source), "diff", "HEAD", "--binary"])
    manifest = {"source": before, "mltRevision": MLT_REVISION,
                "mltWorktreeSha256": hashlib.sha256(dependency_diff).hexdigest(),
                "patches": {name: hashlib.sha256((SOURCE / "packaging/patches" / name).read_bytes()).hexdigest()
                            for name in PATCHES},
                "nativeTestsPassed": True, "fullEditorCoverageVerified": False,
                "executable": str(prefix / "bin/kdenlive")}
    stamp.write_text(json.dumps(manifest, indent=2) + "\n")
    print("Installed native MCP build: " + str(prefix / "bin/kdenlive"))
    print("Native tests passed. Live editor workflows and remaining feature contracts still require verification.")
    return 0


def launch(args):
    path = endpoint(args)
    executable = args.root / "install/bin/kdenlive"
    if not executable.is_file():
        raise SetupError("Native MCP executable is absent. Run build; the stock Flatpak has no MCP endpoint.")
    stamp = args.root / "build-manifest.json"
    if not stamp.is_file():
        raise SetupError("Build/test evidence is absent. Run build before launching this installation.")
    manifest = json.loads(stamp.read_text())
    if not manifest.get("nativeTestsPassed") or not same_source(manifest.get("source"), fingerprint()):
        raise SetupError("Source differs from the tested build. Rebuild before launching this installation.")
    check_runtime(args.root)
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    directory = path.parent.lstat()
    if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != os.getuid() or directory.st_mode & 0o077:
        raise SetupError("Socket directory must be a real directory owned by you with mode 0700: " + str(path.parent))
    if os.path.lexists(path):
        raise SetupError("Endpoint already exists; inspect its owner or choose another name. It will not be removed: " + str(path))
    env = runtime_env(args.root, isolated=args.isolated)
    # Qt resolves relative local-server names under TMPDIR. The connector uses
    # this exact absolute path, avoiding both Flatpak's private /tmp and guesses.
    env["TMPDIR"] = str(path.parent)
    argv = [str(executable), "--no-welcome", "--mcp-socket", args.socket_name]
    if args.project:
        argv.append(str(Path(args.project).expanduser().resolve(strict=True)))
    print("MCP endpoint: " + str(path), file=sys.stderr, flush=True)
    os.execve(executable, argv, env)


def config(args):
    print(json.dumps({"mcpServers": {"kdenlive": {
        "command": sys.executable,
        "args": ["-B", str(SOURCE / "src/mcp/kdenlive-mcp.py"), "--socket", str(endpoint(args)), "--timeout", "30"]
    }}}, indent=2))
    return 0


def probe(args):
    spec = importlib.util.spec_from_file_location("kdenlive_mcp", SOURCE / "src/mcp/kdenlive-mcp.py")
    bridge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bridge)
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "kdenlive-linux-probe", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        *[{"jsonrpc": "2.0", "id": index, "method": "tools/call", "params": {"name": name, "arguments": {}}}
          for index, name in enumerate(("get_info", "get_capabilities", "get_state"), start=3)],
    ]
    inputs = b"".join(json.dumps(request, separators=(",", ":")).encode() + b"\n" for request in requests)
    output, diagnostics = io.BytesIO(), io.StringIO()
    code = bridge.run_proxy(io.BytesIO(inputs), output, diagnostics,
                            lambda timeout: bridge.connect_local(str(endpoint(args)), timeout), 30)
    if code:
        raise SetupError(diagnostics.getvalue().strip() or "Native MCP probe failed.")
    replies = {value["id"]: value for value in map(json.loads, output.getvalue().splitlines()) if "id" in value}
    if set(replies) != {1, 2, 3, 4, 5}:
        raise SetupError("Probe did not receive every native response.")
    for value in replies.values():
        if "error" in value or value.get("result", {}).get("isError"):
            raise SetupError("Native discovery failed: " + json.dumps(value))
    tools = replies[2]["result"].get("tools", [])
    names = [tool.get("name") for tool in tools]
    if not names or len(set(names)) != len(names) or not {"get_info", "get_capabilities", "get_state"} <= set(names):
        raise SetupError("Native tool catalog is empty, ambiguous or missing required discovery tools.")
    info = replies[3]["result"].get("structuredContent", {})
    if info.get("application") != "Kdenlive" or not info.get("ok"):
        raise SetupError("Endpoint did not identify itself as the native Kdenlive MCP server.")
    for index in (4, 5):
        if not replies[index]["result"].get("structuredContent", {}).get("ok"):
            raise SetupError("Native capabilities/state did not report a successful read.")
    print(json.dumps({"connected": True, "editor": info, "toolCount": len(names),
                      "stateRead": True, "editsPerformed": False,
                      "fullEditorCoverageVerified": False}, indent=2))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=lambda value: Path(value).expanduser().resolve(), default=SOURCE / "build-mcp-linux")
    parser.add_argument("--socket-name", default="kdenlive-mcp-local")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("doctor", "config", "probe"):
        commands.add_parser(name)
    builder = commands.add_parser("build")
    builder.add_argument("--jobs", type=int, choices=range(1, 65), default=2, metavar="1..64")
    launcher = commands.add_parser("launch")
    launcher.add_argument("--isolated", action="store_true", help="Use disposable configuration/data/cache under --root.")
    launcher.add_argument("--project")
    args = parser.parse_args(argv)
    try:
        if not sys.platform.startswith("linux"):
            raise SetupError("This helper targets Linux. Use the standalone bridge on Windows.")
        return globals()[args.command](args)
    except (SetupError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print("Kdenlive MCP: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
