#!/usr/bin/env python3
"""Compile, package, boot and deploy QNX binaries over SSH.

Works on Windows (qnxsdp-env.bat) and Linux/macOS (qnxsdp-env.sh).

Targets (--target):
  vbox   VirtualBox VM managed with mkqnximage   (default workspace: <root>/vm-test)
  qemu   QEMU VM managed with mkqnximage         (default workspace: <root>/qemu-test)
  pi     A real board that is already running; deploy only, needs --host

Typical use, from the project root:

  # compile + copy + run in the background on the VirtualBox VM
  python scripts/auto_qnx.py src/driver/dummy_driver.c -l slog2 --mode daemon

  # build the boot image with the driver inside, boot it, check the driver
  python scripts/auto_qnx.py src/driver/dummy_driver.c -l slog2 --rebuild-image --mode verify

  # same on QEMU (the QEMU command with the network fix is used with --qemu-launch custom)
  python scripts/auto_qnx.py src/driver/dummy_driver.c -l slog2 --target qemu \
      --qemu-launch custom --rebuild-image --mode verify

  # a Raspberry Pi that is already booted
  python scripts/auto_qnx.py src/driver/dummy_driver.c -l slog2 --target pi \
      --arch aarch64le --host 192.168.1.50 --mode daemon

Setup:
  * Set QNX_SDP_ENV to your qnxsdp-env.bat / qnxsdp-env.sh, or pass --sdp-env.
    If the SDP environment is already loaded in your shell (QNX_TARGET is set)
    neither is needed.
  * The first --rebuild-image in an empty workspace creates it:
    mkqnximage --type=<vbox|qemu> --ssh-ident=<your .pub> --build
  * Snippets live in <root>/bsp/*.custom. In bsp/ifs_files.custom (and any other
    snippet) these words are filled in at sync time, so no machine-specific path
    is committed:
      @BIN@         full path of the compiled binary
      @NAME@        the binary name
      @ARCH@        x86_64 or aarch64le
      @BINDIR@      folder holding the compiled binaries for this architecture
                    (where --extra programs are written)
      @QNX_TARGET@  the SDP's target folder (from $QNX_TARGET, or found next to
                    the SDP environment script)
  * -D NAME[=VALUE] defines a preprocessor macro for every compile, e.g. -D USE_THREAD_POOL.
  * --extra NAME:LIBS:SOURCE compiles one more program into the same bin folder so
    snippets can put it in the image, e.g. --extra ham_guard:ham:src/driver/ham_guard.c
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence

IS_WINDOWS = os.name == "nt"
PROJECT_ROOT = Path(__file__).resolve().parent.parent  # this file is <root>/scripts/

ARCH_COMPILERS = {
    "x86_64": "gcc_ntox86_64",
    "aarch64le": "gcc_ntoaarch64le",
}

# type = value for mkqnximage --type (None: not managed by mkqnximage)
TARGETS = {
    "vbox": {"type": "vbox", "archs": ("x86_64",), "workspace": "vm-test"},
    "qemu": {"type": "qemu", "archs": ("x86_64", "aarch64le"), "workspace": "qemu-test"},
    "pi": {"type": None, "archs": ("aarch64le",), "workspace": None},
}

WINDOWS_SSH_MACS = "hmac-sha2-256-etm@openssh.com"  # workaround for the Win32-OpenSSH "Corrupted MAC" bug
DEFAULT_BOOT_TIMEOUT = 120
CUSTOM_QEMU_PORT = 2222
IP_PATTERN = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b")
SAFE_NAME = re.compile(r"^[A-Za-z0-9_.-]+$")
SAFE_LIB = re.compile(r"^[A-Za-z0-9_+.-]+$")
SAFE_DEFINE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_.+-]*)?$")
PLACEHOLDERS = ("@BIN@", "@NAME@", "@ARCH@", "@BINDIR@", "@QNX_TARGET@")


PERMISSION_HINT = ("Hint: the target did not accept your key. The image probably has no key in it. "
                   "Rebuild with --rebuild-image (it installs ~/.ssh/id_ed25519.pub, or the file "
                   "given with --ssh-ident), and use --identity if your private key has another name.")


class CommandError(RuntimeError):
    """Raised when something the script depends on fails."""


# --------------------------------------------------------------------------- args

def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Compile QNX binaries, optionally rebuild the boot image, and deploy over SSH.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("sources", nargs="*", type=Path,
                   help="C/C++ source files to compile (not needed with --rebuild-image, "
                        "--mode verify or --stop-vm).")
    p.add_argument("-o", "--output", help="Name of the compiled binary "
                   "(default: my_driver when rebuilding the image, otherwise the first source's name).")
    p.add_argument("-l", "--libs", nargs="*", default=[],
                   help="Libraries to link, without the -l prefix, e.g. -l slog2 socket.")
    p.add_argument("-I", "--include", action="append", type=Path, default=[], metavar="DIR",
                   help="Extra header folder for the compiler (repeatable). <root>/include is always added if it exists.")
    p.add_argument("-D", "--define", action="append", default=[], metavar="NAME[=VALUE]",
                   help="Preprocessor define for every compile (repeatable), e.g. -D USE_THREAD_POOL.")
    p.add_argument("--extra", action="append", default=[], metavar="NAME:LIBS:SOURCE",
                   help="Also compile SOURCE into <bin>/<arch>/NAME, linking the comma-separated LIBS "
                        "(may be empty), so a snippet can add it to the image via @BINDIR@. "
                        "Repeatable. Example: ham_guard:ham:src/driver/ham_guard.c")
    p.add_argument("--target", choices=TARGETS, default="vbox", help="Target kind (default: vbox).")
    p.add_argument("--arch", choices=ARCH_COMPILERS,
                   help="CPU architecture (default: x86_64, or aarch64le for --target pi).")

    g = p.add_argument_group("locations")
    g.add_argument("--root", type=Path, default=PROJECT_ROOT, help=f"Project root (default: {PROJECT_ROOT}).")
    g.add_argument("--target-dir", type=Path, help="mkqnximage workspace (default: <root>/vm-test or <root>/qemu-test).")
    g.add_argument("--snippets-dir", type=Path, help="Folder with *.custom snippets (default: <root>/bsp).")
    g.add_argument("--bin-dir", type=Path, help="Folder for compiled binaries (default: <root>/bin).")
    g.add_argument("--sdp-env", type=Path, help="qnxsdp-env.bat / qnxsdp-env.sh (default: $QNX_SDP_ENV).")

    g = p.add_argument_group("image and VM")
    g.add_argument("--rebuild-image", action="store_true",
                   help="Sync snippets and run mkqnximage --build (creates the workspace the first time).")
    g.add_argument("--ssh-ident", type=Path,
                   help="Public key to put in the image on every build (default: ~/.ssh/id_ed25519.pub).")
    g.add_argument("--qemu-launch", choices=("stock", "custom"), default="stock",
                   help="stock: mkqnximage --run. custom: our own QEMU command with a network card "
                        "and port forward (needed if mkqnximage gives QEMU no network, seen on Windows).")
    g.add_argument("--qemu-bin", default="qemu-system-x86_64", help="QEMU program for --qemu-launch custom.")
    g.add_argument("--stop-vm", action="store_true", help="Stop the VM and exit.")

    g = p.add_argument_group("connection")
    g.add_argument("--host", help="Target IP/hostname (default: found with mkqnximage --getip; "
                                  "127.0.0.1 for --qemu-launch custom; required for --target pi).")
    g.add_argument("--port", type=int, help="SSH port (default: 22, or 2222 for --qemu-launch custom).")
    g.add_argument("--user", default="root", help="SSH user (default: root).")
    g.add_argument("--identity", type=Path, help="Private key file to use for ssh/scp (-i).")
    g.add_argument("--macs", help="Override the SSH MAC list (default: set on Windows only).")
    g.add_argument("--boot-timeout", type=int, default=DEFAULT_BOOT_TIMEOUT,
                   help=f"Seconds to wait for SSH (default: {DEFAULT_BOOT_TIMEOUT}).")
    g.add_argument("--run-timeout", type=int, default=30,
                   help="Seconds before a oneshot run is abandoned (default: 30). Use --mode daemon "
                        "for programs that do not exit.")

    p.add_argument("--mode", choices=("oneshot", "daemon", "verify", "image-only"), default="oneshot",
                   help="oneshot: copy to /tmp and run in the foreground; daemon: copy, restart in the "
                        "background; verify (alias image-only): only check the boot-started process "
                        "and recent slog2 output.")

    args = p.parse_args(argv)
    profile = TARGETS[args.target]

    args.root = args.root.expanduser().resolve()
    args.arch = args.arch or profile["archs"][0]
    if args.arch not in profile["archs"]:
        p.error(f"--target {args.target} supports {', '.join(profile['archs'])}, not {args.arch}.")
    args.bin_dir = (args.bin_dir or args.root / "bin").expanduser().resolve()
    args.snippets_dir = (args.snippets_dir or args.root / "bsp").expanduser().resolve()
    if profile["workspace"]:
        args.target_dir = (args.target_dir or args.root / profile["workspace"]).expanduser().resolve()
    else:
        args.target_dir = None
    if args.ssh_ident:
        args.ssh_ident = args.ssh_ident.expanduser().resolve()
    if args.identity:
        args.identity = args.identity.expanduser().resolve()

    managed = profile["type"] is not None
    verify_like = args.mode in ("verify", "image-only")
    if not args.sources and not (args.rebuild_image or verify_like or args.stop_vm):
        p.error("Provide source file(s), or use --rebuild-image, --mode verify or --stop-vm.")
    if args.rebuild_image and not managed:
        p.error("--rebuild-image is not supported for --target pi yet.")
    if args.stop_vm and not managed:
        p.error("--stop-vm only applies to vbox and qemu targets.")
    if args.qemu_launch == "custom" and (args.target != "qemu" or args.arch != "x86_64"):
        p.error("--qemu-launch custom only works with --target qemu --arch x86_64.")
    if not managed and not args.host:
        p.error("--target pi needs --host <ip>.")
    if not SAFE_NAME.fullmatch(args.user):
        p.error("--user contains unsupported characters.")
    for lib in args.libs:
        if not SAFE_LIB.fullmatch(lib):
            p.error(f"Unsupported library name: {lib}")

    if args.port is None:
        args.port = CUSTOM_QEMU_PORT if args.qemu_launch == "custom" else 22
    if args.qemu_launch == "custom" and not args.host:
        args.host = "127.0.0.1"
    if not 1 <= args.port <= 65535:
        p.error("--port must be between 1 and 65535.")
    if args.boot_timeout <= 0 or args.run_timeout <= 0:
        p.error("Timeouts must be positive.")

    include_dirs = [d.expanduser().resolve() for d in args.include]
    default_include = args.root / "include"
    if default_include.is_dir() and default_include not in include_dirs:
        include_dirs.insert(0, default_include)
    for d in include_dirs:
        if not d.is_dir():
            p.error(f"Include folder not found: {d}")
    args.include = include_dirs

    for define in args.define:
        if not SAFE_DEFINE.fullmatch(define):
            p.error(f"Unsupported -D value: {define} (use NAME or NAME=VALUE with simple characters).")

    extras = []
    for spec in args.extra:
        parts = spec.split(":", 2)  # the source is last, so a Windows drive letter in it is fine
        if len(parts) != 3:
            p.error(f"--extra needs NAME:LIBS:SOURCE, got: {spec}")
        extra_name, extra_libs, extra_src = parts
        if not SAFE_NAME.fullmatch(extra_name) or extra_name in {".", ".."}:
            p.error(f"--extra: unsupported program name: {extra_name}")
        lib_list = [lib for lib in extra_libs.split(",") if lib]
        for lib in lib_list:
            if not SAFE_LIB.fullmatch(lib):
                p.error(f"--extra: unsupported library name: {lib}")
        extra_path = Path(extra_src).expanduser().resolve()
        if not extra_path.is_file():
            p.error(f"--extra: source file not found: {extra_path}")
        extras.append((extra_name, [extra_path], lib_list))
    args.extra = extras

    resolved = []
    for source in args.sources:
        source = source.expanduser().resolve()
        if not source.is_file():
            p.error(f"Source file not found: {source}")
        resolved.append(source)
    args.sources = resolved

    if not args.output:
        if args.rebuild_image or verify_like or not args.sources:
            args.output = "my_driver"
        else:
            args.output = args.sources[0].stem
    if not SAFE_NAME.fullmatch(args.output) or args.output in {".", ".."}:
        p.error("--output may only contain letters, numbers, dots, underscores and hyphens.")

    args.sdp_env = find_sdp_env(args.sdp_env, p)
    return args


def find_sdp_env(explicit: Path | None, parser: argparse.ArgumentParser) -> Path | None:
    """Return the SDP environment script, or None if the environment is already loaded."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("QNX_SDP_ENV"):
        candidates.append(Path(os.environ["QNX_SDP_ENV"]))
    name = "qnxsdp-env.bat" if IS_WINDOWS else "qnxsdp-env.sh"
    candidates.append(Path.home() / "qnx800" / name)
    for candidate in candidates:
        candidate = candidate.expanduser()
        if candidate.is_file():
            return candidate.resolve()
    if explicit or os.environ.get("QNX_SDP_ENV"):
        parser.error("The QNX SDP environment script was not found. Check --sdp-env / QNX_SDP_ENV.")
    if os.environ.get("QNX_TARGET"):
        return None  # shell already has the SDP environment
    parser.error(f"Could not find {name}. Set QNX_SDP_ENV or pass --sdp-env.")


# ------------------------------------------------------------------ running things

def qpath(value: object) -> str:
    """Quote a path or word for the shell that will run it."""
    return f'"{value}"' if IS_WINDOWS else shlex.quote(str(value))


def qnx_shell(command: str, sdp_env: Path | None, cwd: Path | None) -> list[str] | str:
    """Return something subprocess can run: the command with the SDP environment loaded."""
    if IS_WINDOWS:
        parts = []
        if sdp_env:
            parts.append(f'call "{sdp_env}" >nul')
        if cwd:
            parts.append(f'cd /d "{cwd}"')
        parts.append(command)
        # A single string: with a list Python would escape the inner quotes as \" and cmd.exe cannot parse that.
        return f'cmd.exe /d /s /c "{" && ".join(parts)}"'
    parts = []
    if sdp_env:
        parts.append(f"source {shlex.quote(str(sdp_env))} >/dev/null")
    if cwd:
        parts.append(f"cd {shlex.quote(str(cwd))}")
    parts.append(command)
    return ["bash", "-c", " && ".join(parts)]


def format_failure(label: str, command: str, result: subprocess.CompletedProcess[str]) -> str:
    lines = [f"{label} failed (exit code {result.returncode}):", f"  {command}"]
    if result.stdout.strip():
        lines += ["Stdout:", result.stdout.rstrip()]
    if result.stderr.strip():
        lines += ["Stderr:", result.stderr.rstrip()]
    if "Permission denied" in result.stderr:
        lines.append(PERMISSION_HINT)
    return "\n".join(lines)


def run_qnx(command: str, sdp_env: Path | None, cwd: Path | None = None, check: bool = True,
            timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(qnx_shell(command, sdp_env, cwd), capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired:
        result = subprocess.CompletedProcess(command, 124, "", f"timed out after {timeout} s")
    if check and result.returncode != 0:
        raise CommandError(format_failure("QNX command", command, result))
    return result


def run_host(command: Sequence[str], check: bool = True,
             timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(list(command), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout.decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        raise CommandError(
            f"No result after {timeout} s: {' '.join(command)}\n"
            f"Partial output:\n{out}\n"
            "If the program is meant to keep running, use --mode daemon.") from None
    except FileNotFoundError as exc:
        raise CommandError(f"Program not found: {command[0]} ({exc})") from None
    if check and result.returncode != 0:
        raise CommandError(format_failure("Host command", " ".join(command), result))
    return result


# ------------------------------------------------------------------- ssh helpers

def ssh_options(args: argparse.Namespace) -> list[str]:
    opts = ["-o", "BatchMode=yes",            # fail with an error instead of waiting for a password
            "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=accept-new"]
    macs = args.macs or (WINDOWS_SSH_MACS if IS_WINDOWS else None)
    if macs:
        opts += ["-o", f"MACs={macs}"]
    if args.identity:
        opts += ["-i", str(args.identity)]
    return opts


def ssh_command(args: argparse.Namespace, host: str) -> list[str]:
    return ["ssh", "-p", str(args.port), *ssh_options(args), f"{args.user}@{host}"]


def scp_command(args: argparse.Namespace) -> list[str]:
    return ["scp", "-P", str(args.port), *ssh_options(args)]


def forget_host_key(host: str, port: int) -> None:
    """A rebuilt image has new host keys; drop the old entry so ssh does not refuse to connect."""
    names = [host] if port == 22 else [f"[{host}]:{port}"]
    for name in names:
        run_host(["ssh-keygen", "-R", name], check=False)


def port_is_open(host: str, port: int) -> bool:
    """True only when an SSH server answers with its banner.

    A plain "port open" test is not enough: QEMU's port forward accepts connections
    as soon as QEMU starts, long before the guest's sshd is running.
    """
    try:
        with socket.create_connection((host, port), timeout=2) as conn:
            conn.settimeout(3)
            return conn.recv(4) == b"SSH-"
    except OSError:
        return False


def find_ip(output: str) -> str | None:
    for candidate in reversed(IP_PATTERN.findall(output)):
        if all(0 <= int(part) <= 255 for part in candidate.split(".")):
            return candidate
    return None


def get_target_ip(workspace: Path, sdp_env: Path | None) -> str | None:
    result = run_qnx("mkqnximage --getip", sdp_env, cwd=workspace, check=False, timeout=15)
    return find_ip(result.stdout + "\n" + result.stderr)


# ---------------------------------------------------------------- workspace / image

def read_options(workspace: Path) -> dict[str, str]:
    path = workspace / "local" / "options"
    values: dict[str, str] = {}
    if path.is_file():
        for line in path.read_text(errors="replace").splitlines():
            match = re.match(r"^(\w+)='(.*)'$", line.strip())
            if match:
                values[match.group(1)] = match.group(2)
    return values


def check_workspace(args: argparse.Namespace) -> bool:
    """Return True if the workspace is configured. Raise if it belongs to another target."""
    options = read_options(args.target_dir)
    if not options:
        return False
    expected = TARGETS[args.target]["type"]
    if options.get("OPT_TYPE") != expected:
        raise CommandError(f"{args.target_dir} is a '{options.get('OPT_TYPE')}' workspace, "
                           f"but --target {args.target} needs '{expected}'. Use --target-dir.")
    if options.get("OPT_ARCH") != args.arch:
        raise CommandError(f"{args.target_dir} is built for {options.get('OPT_ARCH')}, "
                           f"but --arch is {args.arch}. Use another --target-dir.")
    return True


def qnx_target_dir(args: argparse.Namespace) -> Path:
    """The SDP's target folder (the one that holds x86_64/ and aarch64le/)."""
    candidates = []
    if os.environ.get("QNX_TARGET"):
        candidates.append(Path(os.environ["QNX_TARGET"]))
    if args.sdp_env:
        candidates.append(args.sdp_env.parent / "target" / "qnx")
    for candidate in candidates:
        if candidate.is_dir():
            return candidate.resolve()
    raise CommandError("A snippet uses @QNX_TARGET@ but the SDP target folder was not found. "
                       "Set QNX_TARGET (load the SDP environment) or pass --sdp-env.")


def render(text: str, args: argparse.Namespace, binary: Path, name: str) -> str:
    text = text.replace("@BIN@", binary.as_posix()).replace("@NAME@", name)
    text = text.replace("@ARCH@", args.arch)
    text = text.replace("@BINDIR@", (args.bin_dir / args.arch).as_posix())
    if "@QNX_TARGET@" in text:
        text = text.replace("@QNX_TARGET@", qnx_target_dir(args).as_posix())
    return text


def sync_snippets(args: argparse.Namespace, binary: Path) -> None:
    if not args.snippets_dir.is_dir():
        raise CommandError(f"Snippets folder not found: {args.snippets_dir}")
    if not binary.is_file():
        raise CommandError(f"The image needs the compiled binary, but it is missing: {binary}\n"
                           "Pass the source file(s) so it is compiled first.")
    dest = args.target_dir / "local" / "snippets"
    dest.mkdir(parents=True, exist_ok=True)

    sources = {p.name: p for p in args.snippets_dir.glob("*.custom") if p.is_file()}
    keep = set(sources) | {"ifs_files.custom"}
    for old in dest.glob("*.custom"):
        if old.name not in keep and old.is_file():
            old.unlink()

    template = sources.pop("ifs_files.custom", None)
    generated = f"[perms=0755] /proc/boot/{args.output} = {binary.as_posix()}\n"
    if template:
        text = template.read_text()
        if "@BIN@" in text:
            generated = render(text, args, binary, args.output)
        else:
            print(f"[warn] {template} has no @BIN@ placeholder, so it was not used. "
                  "A line with the right path was generated instead.")
    (dest / "ifs_files.custom").write_text(generated)

    for name, src in sources.items():
        text = src.read_text()
        if any(placeholder in text for placeholder in PLACEHOLDERS):
            (dest / name).write_text(render(text, args, binary, args.output))
        else:
            shutil.copy2(src, dest / name)
    print(f"Synced {len(sources) + 1} snippet(s) into {dest}")


def build_image(args: argparse.Namespace, configured: bool) -> None:
    command = "mkqnximage"
    if not configured:
        command += f" --type={TARGETS[args.target]['type']}"
        if args.arch != "x86_64":
            command += f" --arch={args.arch}"
    # The key is NOT remembered in the workspace options (OPT_SSH_IDENT stays 'prompt'),
    # so it has to be given on every build or the rebuilt image has no key in it.
    key = Path(args.ssh_ident or Path.home() / ".ssh" / "id_ed25519.pub")
    if not key.is_file():
        raise CommandError(f"Public key not found: {key}\n"
                           "Create one with: ssh-keygen -t ed25519   or pass --ssh-ident <file>.pub")
    # mkqnximage is a shell script: backslashes in the path get eaten, so use forward slashes.
    command += f" --ssh-ident={qpath(key.as_posix())}"
    command += " --build"
    args.target_dir.mkdir(parents=True, exist_ok=True)
    print(f"Building the image ({command})...")
    run_qnx(command, args.sdp_env, cwd=args.target_dir)
    print("Image build complete.")


# ------------------------------------------------------------------ compile

def build_binary(args: argparse.Namespace) -> Path | None:
    if not args.sources:
        return None
    return compile_program(args, args.sources, args.libs, args.output)


def build_extras(args: argparse.Namespace) -> None:
    for name, sources, libs in args.extra:
        compile_program(args, sources, libs, name)


def compile_program(args: argparse.Namespace, source_files: Sequence[Path],
                    lib_names: Sequence[str], name: str) -> Path:
    out_dir = args.bin_dir / args.arch
    out_dir.mkdir(parents=True, exist_ok=True)
    binary = out_dir / name
    sources = " ".join(qpath(s) for s in source_files)
    libs = " ".join(f"-l{lib}" for lib in lib_names)
    includes = " ".join(f"-I {qpath(d)}" for d in args.include)
    defines = " ".join(f"-D{d}" for d in args.define)
    command = f"qcc -V{ARCH_COMPILERS[args.arch]} {includes} {defines} {sources} {libs} -o {qpath(binary)}"
    print(f"Compiling for {args.arch} -> {binary}...")
    result = run_qnx(command, args.sdp_env)
    for text in (result.stdout, result.stderr):
        if text.strip():
            print(text.rstrip())
    if not binary.is_file():
        raise CommandError(f"The compiler reported success but {binary} was not created.")
    print("Compile successful.")
    return binary


# ------------------------------------------------------------------ VM control

def qemu_custom_command(args: argparse.Namespace) -> str:
    return (f"{qpath(args.qemu_bin)} -smp 2 --cpu max -m 1G "
            "-drive file=output/disk-qemu.vmdk,if=ide,id=drv0 -pidfile output/qemu.pid "
            "-nographic -kernel output/ifs.bin -serial mon:stdio "
            f"-netdev user,id=net0,hostfwd=tcp::{args.port}-:22 -device virtio-net-pci,netdev=net0")


def start_custom_qemu(args: argparse.Namespace) -> None:
    for needed in ("output/ifs.bin", "output/disk-qemu.vmdk"):
        if not (args.target_dir / needed).is_file():
            raise CommandError(f"Missing {args.target_dir / needed}. Build the image first (--rebuild-image).")
    log_path = args.target_dir / "output" / "qemu-console.log"
    print(f"Starting QEMU (console output: {log_path})...")
    kwargs: dict = {}
    if IS_WINDOWS:
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS
    else:
        kwargs["start_new_session"] = True
    with open(log_path, "w") as log:
        proc = subprocess.Popen(qnx_shell(qemu_custom_command(args), args.sdp_env, args.target_dir),
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, **kwargs)
    time.sleep(3)
    if proc.poll() is not None:
        tail = log_path.read_text(errors="replace")[-1500:]
        raise CommandError(f"QEMU exited straight away (code {proc.returncode}). Console log:\n{tail}")


def stop_vm(args: argparse.Namespace) -> None:
    if args.qemu_launch == "custom":
        pid_file = args.target_dir / "output" / "qemu.pid"
        if not pid_file.is_file():
            print("No qemu.pid file; QEMU is probably not running.")
            return
        pid = pid_file.read_text().strip()
        if not pid.isdigit():
            raise CommandError(f"Unexpected contents in {pid_file}")
        if IS_WINDOWS:
            run_host(["taskkill", "/PID", pid, "/T", "/F"], check=False)
        else:
            try:
                os.kill(int(pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
        print(f"Stopped QEMU (pid {pid}).")
    else:
        run_qnx("mkqnximage --stop", args.sdp_env, cwd=args.target_dir, check=False)
        print("Stop requested (mkqnximage --stop).")


def ensure_target_running(args: argparse.Namespace) -> None:
    if args.qemu_launch == "custom":
        if port_is_open(args.host, args.port):
            print(f"Target already answering at {args.host}:{args.port}.")
        else:
            start_custom_qemu(args)
        return
    host = args.host or get_target_ip(args.target_dir, args.sdp_env)
    if host and port_is_open(host, args.port):
        print(f"Target already online at {host}:{args.port} (skipping --run).")
        return
    print("Starting the VM (mkqnximage --run)...")
    run_qnx("mkqnximage --run", args.sdp_env, cwd=args.target_dir)


def wait_for_ssh(args: argparse.Namespace) -> str:
    managed_lookup = args.target != "pi" and args.qemu_launch != "custom"
    deadline = time.monotonic() + args.boot_timeout
    last = args.host
    while time.monotonic() < deadline:
        host = args.host or (get_target_ip(args.target_dir, args.sdp_env) if managed_lookup else None)
        if host:
            last = host
            if port_is_open(host, args.port):
                return host
        print(f"  waiting for SSH{f' at {last}:{args.port}' if last else ''}...")
        time.sleep(3)
    raise CommandError(f"SSH was not reachable after {args.boot_timeout} s.")


# ------------------------------------------------------------------ main

def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        for tool in ("ssh", "scp", "ssh-keygen"):
            if shutil.which(tool) is None:
                raise CommandError(f"{tool} was not found on PATH.")
        managed = args.target_dir is not None

        if args.stop_vm:
            stop_vm(args)
            return 0

        binary = build_binary(args)
        build_extras(args)

        rebuilt = False
        if managed:
            configured = check_workspace(args)
            if args.rebuild_image:
                if binary is None:
                    binary = args.bin_dir / args.arch / args.output
                sync_snippets(args, binary)
                print("Stopping any running VM before the rebuild...")
                stop_vm(args)
                build_image(args, configured)
                rebuilt = True
            elif not configured:
                raise CommandError(f"{args.target_dir} has no mkqnximage workspace yet. "
                                   "Run once with --rebuild-image to create and build it.")
            ensure_target_running(args)

        print("Waiting for SSH...")
        host = wait_for_ssh(args)
        print(f"Target reachable at {host}:{args.port}")
        if rebuilt:
            forget_host_key(host, args.port)

        ssh = ssh_command(args, host)

        if args.mode in ("verify", "image-only"):
            print("\nChecking the target process and recent slog2 output...")
            remote = (f"pidin | grep -F -- {shlex.quote(args.output)}; "
                      "echo '--- recent slog2 ---'; slog2info | tail -n 10")
            result = run_host([*ssh, remote], check=False)
            print("------------ Target status ------------")
            print(result.stdout or result.stderr or "(no output)")
            if "Permission denied" in result.stderr:
                print(PERMISSION_HINT)
            print("---------------------------------------")
            return 0 if result.returncode == 0 else 1

        if binary is None or not binary.is_file():
            raise CommandError("There is no compiled binary to deploy. Provide source files.")

        print(f"\nCopying {binary.name} to {host}:/tmp/ ...")
        run_host([*scp_command(args), str(binary), f"{args.user}@{host}:/tmp/"])

        name = shlex.quote(args.output)
        if args.mode == "oneshot":
            print("Running the binary on the target...")
            result = run_host([*ssh, f"chmod +x /tmp/{name} && /tmp/{name}"], timeout=args.run_timeout)
            print("------------ Result ------------")
            print(result.stdout)
            print("--------------------------------")
        else:
            print(f"Restarting {args.output} in the background on the target...")
            remote = (f"slay -f {name} 2>/dev/null; chmod +x /tmp/{name} && "
                      f"(/tmp/{name} >/dev/null 2>&1 </dev/null &) ; sleep 2; "
                      f"pidin | grep -F -- {name}; echo '--- recent slog2 ---'; slog2info | tail -n 10")
            result = run_host([*ssh, remote])
            print("------------ Daemon status ------------")
            print(result.stdout)
            print("---------------------------------------")
        return 0

    except CommandError as error:
        print(f"\n[ERROR] {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())