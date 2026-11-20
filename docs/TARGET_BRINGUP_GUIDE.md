# QNX 8.0 Target Bring-Up Guide

How to compile the driver, package it into a QNX boot image, boot a target, and check that it runs and heals itself.

There are two ways to do it, and they do exactly the same things:

* **The script** (`scripts/auto_qnx.py`): one command. Recommended for everyday use.
* **The manual steps** (section 5): the commands the script runs. Use them to understand what is happening, or if the script fails.

> **Status.** The script has been tested on Windows with VirtualBox and QEMU (x86_64). Linux and macOS paths are written but not yet tested on real machines. The boot image has not been built for the Raspberry Pi yet (see section 7). If something breaks, please open an issue with the full command and its output.

## 1. One-time setup

You need:

* **QNX SDP 8.0** installed, **plus the High Availability Manager package** (section 1.4).
* **Python 3** (for the script) and the **OpenSSH client** (`ssh`, `scp`, `ssh-keygen`) on your `PATH`.
* **A target to run on.** VirtualBox needs a host-only network with DHCP enabled (VirtualBox: Tools > Network). QEMU needs `qemu-system-x86_64`. A Raspberry Pi needs to be booted and reachable on your network.

### 1.1 Create an SSH key (skip if you already have one)

```
ssh-keygen -t ed25519
```

Press Enter to accept the default location. The public key (`~/.ssh/id_ed25519.pub`) gets built into the target image so you can log in without a password. The private key never leaves your computer and must never be committed.

### 1.2 Tell the tools where the SDP is

The SDP environment script is `qnxsdp-env.bat` on Windows and `qnxsdp-env.sh` on Linux and macOS. Set the path once and the script loads it for you.

| System | Command |
|---|---|
| Windows (then open a new `cmd`) | `setx QNX_SDP_ENV "D:\qnx_downloads\qnxsdp-env.bat"` |
| Linux / macOS (add to `~/.bashrc`) | `export QNX_SDP_ENV=~/qnx800/qnxsdp-env.sh` |

Adjust the path to where the SDP is installed. If you prefer, load the environment yourself in each terminal (`call ...\qnxsdp-env.bat` or `source .../qnxsdp-env.sh`) and the script will detect it.

### 1.3 Windows only: the SSH MAC workaround

The Windows OpenSSH client can fail with `Corrupted MAC on input`. Fix it once by creating `%USERPROFILE%\.ssh\config` (no file extension) with:

```
Host 192.168.56.* 127.0.0.1
    MACs hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com
```

The script applies the same setting on its own. This file is for plain `ssh` and `scp` commands.

### 1.4 Install the High Availability Manager package

The boot image starts QNX's High Availability Manager (HAM), which launches the driver and restarts it if it dies. HAM is **not** part of the default SDP install. Without it the image build fails with a missing-file error.

1. Open **QNX Software Center**, select your SDP 8.0 installation, and install **"High Availability Manager"** (search for "High Availability").
2. Check that it arrived (Windows `cmd`; use `ls` on Linux and macOS):

```
dir /s /b D:\qnx_downloads\target\qnx\x86_64\usr\sbin\ham*
dir /s /b D:\qnx_downloads\target\qnx\x86_64\usr\lib\libham*
dir /s /b D:\qnx_downloads\target\qnx\usr\include\ha\ham.h
```

You should see `ham`, `hamctrl`, `libham.so.2` (and `libham.so`, `libham.a`) and `ham.h`. The same files exist under `aarch64le` for ARM.

## 2. Project layout

```
qnx8-resmgr-driver/
├── include/        shared headers (driver_interface.h is the frozen contract)
├── src/driver/     driver sources, plus ham_guard.c
├── bsp/            *.custom snippets copied into the boot image
├── bin/            compiled binaries (not committed)
├── vm-test/        VirtualBox workspace, created by the script (not committed)
├── qemu-test/      QEMU workspace, created by the script (not committed)
├── scripts/        auto_qnx.py
├── tests/          sensor_client.c
└── docs/
```

`vm-test/`, `qemu-test/` and `bin/` belong in `.gitignore`. They contain generated files and your key.

The snippets in `bsp/` do these jobs:

| File | Purpose |
|---|---|
| `ifs_files.custom` | Puts the driver, `ham`, `hamctrl`, `libham.so.2` and `ham_guard` into the boot image. Uses placeholders for their paths (see below) |
| `post_start.custom` | At the end of boot: starts `ham`, then runs `ham_guard`, which registers the driver so HAM starts and supervises it |
| `system_files.custom`, `ifs_env.custom` | Install the `sshd-auth` helper that the SSH server needs |

The script fills in these placeholders in any snippet when it copies it into the workspace, so no machine-specific path is committed:

| Placeholder | Becomes |
|---|---|
| `@BIN@` | full path of the compiled driver binary |
| `@NAME@` | the driver binary's name |
| `@ARCH@` | `x86_64` or `aarch64le` |
| `@BINDIR@` | the folder with the compiled binaries for that architecture (where `--extra` programs go) |
| `@QNX_TARGET@` | the SDP's target folder (from `$QNX_TARGET`, or found next to the SDP environment script) |

## 3. Quick start with the script

Run everything from the repository root. The same commands work in `cmd`, PowerShell and bash.

**Build the boot image with the driver and HAM inside, boot it, and check that it runs** (VirtualBox is the default target):

```
python scripts/auto_qnx.py src/driver/main_stub.c src/driver/resmgr.c src/driver/concurrency.c -l slog2 -D USE_THREAD_POOL --extra ham_guard:ham:src/driver/ham_guard.c --rebuild-image --mode verify
```

What the pieces mean:

* The three `.c` files are the driver. The first one is compiled together into `bin/<arch>/my_driver`.
* `-l slog2` links the slog2 logging library.
* `-D USE_THREAD_POOL` builds the driver with its thread pool. Without it the driver is single-threaded.
* `--extra ham_guard:ham:src/driver/ham_guard.c` also compiles `ham_guard`, linking `libham`. The format is `NAME:LIBS:SOURCE`.

The first time, this also creates the `vm-test/` workspace. Expect a few minutes.

**Run the client test against whatever is running:**

```
python scripts/auto_qnx.py tests/sensor_client.c --mode oneshot
```

Expect `ALL CHECKS PASSED (0 failures)`.

**Check the self-healing.** Log in (`ssh root@<ip>`, with the address from `mkqnximage --getip`) and run on the target:

```
pidin | grep -E "ham|my_driver"      # both are running after boot
ls /proc/ham                         # shows: sensor
slay -f my_driver
sleep 2
pidin | grep my_driver               # a new process ID: HAM restarted it
```

**Stop the VM:**

```
python scripts/auto_qnx.py --stop-vm
```

**Other targets:**

```
# QEMU (on Windows, add --qemu-launch custom; see "QEMU" in section 5)
python scripts/auto_qnx.py <same sources and options> --target qemu --qemu-launch custom --rebuild-image --mode verify

# A Raspberry Pi that is already booted (deploy only; it cannot build the Pi image yet)
python scripts/auto_qnx.py src/driver/dummy_driver.c -l slog2 --target pi --arch aarch64le --host <pi-ip> --mode daemon
```

**Everything else.** Run `python scripts/auto_qnx.py --help` to list every option, and see the table in section 6.

> **Iterating.** Once HAM supervises the driver, `--mode daemon` is no longer a quick way to push a new binary: killing the boot copy makes HAM restart it at once, and the pushed copy then competes for `/dev/sensor0`. To try a change, run the command above again with `--rebuild-image`. `--mode daemon` still works for programs HAM does not supervise.

> **Thread pool.** The pool is only built with `-D USE_THREAD_POOL`. Whether it becomes the default is still to be decided.

## 4. How the pieces fit together at boot

1. The image's startup script runs the usual QNX services (logging, networking, SSH).
2. `post_start.custom` then runs `ham`, the High Availability Manager, which detaches into the background.
3. `ham_guard sensor /proc/boot/my_driver` connects to HAM (retrying for about ten seconds if HAM is still starting), registers `my_driver` as an entity called `sensor`, adds a condition that fires when it dies and an action that runs it again, and then exits.
4. HAM starts the driver, and from now on restarts it whenever it dies. The registered entity is visible under `/proc/ham`.

`ham` also starts a second process, its own guardian, so two `ham` processes in `pidin` is normal.

## 5. Manual steps (what the script does)

Run these from the repository root in `cmd` (Windows) after loading the SDP environment. Linux and macOS equivalents are in section 5.7.

### 5.1 Compile

Name the driver `my_driver` and the guard `ham_guard`, because those are the names the snippets use:

```
mkdir bin\x86_64 2>nul
qcc -Vgcc_ntox86_64 -I include -DUSE_THREAD_POOL src\driver\main_stub.c src\driver\resmgr.c src\driver\concurrency.c -lslog2 -o bin\x86_64\my_driver
qcc -Vgcc_ntox86_64 -I include src\driver\ham_guard.c -lham -o bin\x86_64\ham_guard
```

For a Raspberry Pi (aarch64), use `-Vgcc_ntoaarch64le` and `bin\aarch64le`.

### 5.2 Create the workspace (first time only)

```
mkdir vm-test
cd vm-test
mkqnximage --type=vbox --ssh-ident="C:/Users/<you>/.ssh/id_ed25519.pub" --build
cd ..
```

Use forward slashes in the key path, or `mkqnximage` drops the backslashes. For QEMU use `--type=qemu` in a folder such as `qemu-test`.

### 5.3 Add the snippets

```
mkdir vm-test\local\snippets
copy /Y bsp\*.custom vm-test\local\snippets\
```

Then open `vm-test\local\snippets\ifs_files.custom` and replace the placeholders by hand, with forward slashes in every path. For example, with the project in `C:/work/qnx8-resmgr-driver` and the SDP in `D:/qnx_downloads`, the file becomes:

```
[perms=0755] /proc/boot/my_driver = C:/work/qnx8-resmgr-driver/bin/x86_64/my_driver
[perms=0755] /proc/boot/ham_guard = C:/work/qnx8-resmgr-driver/bin/x86_64/ham_guard
[perms=0755] /proc/boot/ham = D:/qnx_downloads/target/qnx/x86_64/usr/sbin/ham
[perms=0755] /proc/boot/hamctrl = D:/qnx_downloads/target/qnx/x86_64/usr/sbin/hamctrl
[perms=0755] /proc/boot/libham.so.2 = D:/qnx_downloads/target/qnx/x86_64/usr/lib/libham.so.2
```

`post_start.custom` needs no editing.

### 5.4 Build and boot (VirtualBox)

```
cd vm-test
mkqnximage --ssh-ident="C:/Users/<you>/.ssh/id_ed25519.pub" --build
mkqnximage --run
mkqnximage --getip
```

**Pass `--ssh-ident` on every build.** The key is not remembered, so an image built without it has no key and ssh asks for a password. Stop the VM when done with `mkqnximage --stop`.

### 5.5 QEMU

On Windows, `mkqnximage --run` has started QEMU without a network card, so there is nothing to ssh to. Start QEMU by hand from the QEMU workspace, with a network card and a forward from port 2222 on your computer to port 22 in the VM:

```
qemu-system-x86_64 -smp 2 --cpu max -m 1G -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 -pidfile output/qemu.pid -nographic -kernel output/ifs.bin -serial mon:stdio -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0
```

Leave that window open, since it is the VM's console. Press Ctrl+A then X to quit. Connect with `ssh -p 2222 root@127.0.0.1`, and copy with `scp -P 2222` (capital P). Linux hosts may not need any of this, so try plain `mkqnximage --run` first.

### 5.6 Check the driver and iterate

Connect: `ssh root@<ip>` (use the address from `mkqnximage --getip`). Then on the target:

```
pidin | grep -E "ham|my_driver"
pidin -p my_driver threads
ls /proc/ham
slog2info -w -b dummy_driver
```

The log buffer name is whatever the driver registers with slog2. Test it with `tests/sensor_client.c` as in section 3, and test the restart as shown there. To simulate a crash instead of a clean kill:

```
kill -SEGV $(pidin | grep my_driver | awk '{print $1}' | head -1)
```

To try a change, rebuild the image (5.4). Do not copy a binary over the boot copy while HAM is supervising it (see the note in section 3).

### 5.7 Linux and macOS equivalents

| Windows `cmd` | Linux / macOS |
|---|---|
| `call D:\...\qnxsdp-env.bat` | `source ~/qnx800/qnxsdp-env.sh` |
| `mkdir bin\x86_64 2>nul` | `mkdir -p bin/x86_64` |
| `copy /Y bsp\*.custom vm-test\local\snippets\` | `cp bsp/*.custom vm-test/local/snippets/` |
| `dir /s /b <path>\ham*` | `ls <path>/ham*` |
| Paths with `\` | Paths with `/` |
| MAC workaround needed | Not needed |

## 6. Troubleshooting

| Message or symptom | Likely cause | What to do |
|---|---|---|
| Image build fails with a missing-file error naming `ham`, `hamctrl` or `libham` | The High Availability Manager package is not installed | Install it (section 1.4) |
| Image build fails naming `ham_guard` | The image was built without `--extra` | Add `--extra ham_guard:ham:src/driver/ham_guard.c` |
| `ham` is not running after boot, or `/proc/ham` is empty | `post_start.custom` was not synced, or `ham_guard` could not connect | Check `ls /proc/ham`; look at `slog2info` and the QEMU console for a `ham_guard:` line; rebuild with `--rebuild-image` |
| The driver has only one thread (`pidin -p my_driver threads`) | Built without `-D USE_THREAD_POOL` | Add the option and rebuild |
| Two drivers fighting, or `/dev/sensor0` owned by the wrong copy | A binary was pushed with `--mode daemon` while HAM supervises the driver | Rebuild the image instead (section 3) |
| `Permission denied (publickey...)` | The image has no key | Rebuild with the key: `--rebuild-image` (script), or `--ssh-ident=...` on `mkqnximage --build` |
| `REMOTE HOST IDENTIFICATION HAS CHANGED` | The image was rebuilt and has new host keys | `ssh-keygen -R <ip>` (or `ssh-keygen -R "[127.0.0.1]:2222"` for QEMU). The script does this itself after a rebuild |
| `Corrupted MAC on input` | Windows SSH client bug | Add the config from 1.3 |
| `Connection timed out during banner exchange` (QEMU) | The VM is still booting | Wait and retry. The script waits for the SSH banner automatically |
| QEMU boots but nothing answers on 2222 | `mkqnximage --run` started it without a network | Use `--qemu-launch custom`, or the command in 5.5 |
| `QNX SDP environment script not found` | `QNX_SDP_ENV` is not set | See 1.2, or pass `--sdp-env` |
| `undefined reference to slog2...` | Library not linked | Add `-l slog2` (script) or `-lslog2` (`qcc`) |
| Script waits at "Running the binary" | The program never exits | Use `--mode daemon`, which starts it in the background |
| `no mkqnximage workspace yet` | The workspace folder does not exist | Run once with `--rebuild-image` |
| `Permission denied` after rebuild only on the Pi | The Pi image is built outside this script | Make sure the Pi image contains your public key (or use its password login) |

## 7. Raspberry Pi

The script can deploy to a Pi that is already running (`--target pi`), but it cannot build a Pi boot image. A Pi image needs QNX's board support package (BSP) for the Raspberry Pi 4, available in QNX Software Center, and the same additions as above (HAM, the driver, `ham_guard`) in its build file. That work is in progress. Until it is done, nothing in the Pi image starts the driver at boot.

## 8. Script option reference

`python scripts/auto_qnx.py [options] [source files]`. The same list is printed by `--help`.

| Option | Meaning | Default |
|---|---|---|
| `sources` | C/C++ files to compile into the driver | none (not needed with `--rebuild-image`, `--mode verify`, `--stop-vm`) |
| `-o`, `--output` | Name of the compiled binary | `my_driver` when rebuilding the image, otherwise the first source's name |
| `-l`, `--libs` | Libraries to link, without `-l`: `-l slog2 socket` | none |
| `-I`, `--include` | Extra header folder (repeatable). `<root>/include` is always added | none |
| `-D`, `--define` | Preprocessor define for every compile (repeatable), `NAME` or `NAME=VALUE` | none |
| `--extra` | `NAME:LIBS:SOURCE`: also compile `SOURCE` into `bin/<arch>/NAME`, linking the comma-separated `LIBS` (may be empty). Repeatable | none |
| `--target` | `vbox`, `qemu` or `pi` | `vbox` |
| `--arch` | `x86_64` or `aarch64le` | `x86_64` (`aarch64le` for `pi`) |
| `--rebuild-image` | Sync snippets, build the boot image, boot it. Creates the workspace on first use | off |
| `--mode` | `oneshot`: run in the foreground. `daemon`: restart in the background. `verify` (or `image-only`): only check the running driver and slog2 | `oneshot` |
| `--stop-vm` | Stop the VM and exit | off |
| `--ssh-ident` | Public key to put in the image | `~/.ssh/id_ed25519.pub` |
| `--qemu-launch` | `stock`: `mkqnximage --run`. `custom`: our QEMU command with network and port forward | `stock` |
| `--qemu-bin` | QEMU program for `custom` | `qemu-system-x86_64` |
| `--host` | Target address | found with `mkqnximage --getip`; `127.0.0.1` for custom QEMU; required for `pi` |
| `--port` | SSH port | 22; 2222 for custom QEMU |
| `--user` | SSH user | `root` |
| `--identity` | Private key file for ssh/scp | the SSH default |
| `--macs` | Override the SSH MAC list | set automatically on Windows only |
| `--boot-timeout` | Seconds to wait for SSH | 120 |
| `--run-timeout` | Seconds before a `oneshot` run is abandoned | 30 |
| `--target-dir` | mkqnximage workspace folder | `vm-test` (vbox) or `qemu-test` (qemu) in the project root |
| `--snippets-dir` | Folder with `*.custom` snippets | `bsp` |
| `--bin-dir` | Folder for compiled binaries | `bin` |
| `--root` | Project root | the folder above `scripts/` |
| `--sdp-env` | Path to `qnxsdp-env.bat` / `.sh` | `$QNX_SDP_ENV` |