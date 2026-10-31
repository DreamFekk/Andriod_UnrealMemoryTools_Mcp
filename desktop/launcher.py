#!/usr/bin/env python3
"""Deploy UMT to one Android device and open its UI over ADB. No dependencies."""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import webbrowser

PROJECT = Path(__file__).resolve().parent.parent
REMOTE = "/data/local/tmp/umt-refactored"
DEVICE_PORT = 18765
OUTPUT = "/sdcard/UnrealMemoryTools_Refactored"


class LaunchError(RuntimeError):
    pass


def command(args: list[str], timeout: float = 30) -> str:
    """Execute argument arrays locally; never invoke a Windows command shell."""
    try:
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=timeout, check=False,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except subprocess.TimeoutExpired as exc:
        raise LaunchError(f"Command timed out after {timeout:g}s: {args[0]}") from exc
    except OSError as exc:
        raise LaunchError(f"Could not execute {args[0]}: {exc}") from exc
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise LaunchError(f"Command failed ({result.returncode}): {' '.join(args[:4])}\n{detail}")
    return result.stdout.strip()


def find_adb(explicit: str | None = None) -> str:
    if explicit:
        candidate = Path(explicit).expanduser()
        located = shutil.which(explicit)
        if candidate.is_file():
            return str(candidate.resolve())
        if located:
            return located
        raise LaunchError(f"ADB does not exist: {explicit}")
    for name in ('adb.exe', 'adb'):
        bundled = PROJECT / 'tools' / 'platform-tools' / name
        if bundled.is_file():
            return str(bundled.resolve())
    located = shutil.which("adb")
    if located:
        return located
    roots = [os.environ.get("ANDROID_SDK_ROOT"), os.environ.get("ANDROID_HOME")]
    local = os.environ.get("LOCALAPPDATA")
    if local:
        roots.append(str(Path(local) / "Android" / "Sdk"))
    roots.extend([str(Path.home() / "Android" / "Sdk"), str(Path.home() / "Library" / "Android" / "sdk")])
    for root in filter(None, roots):
        for executable in ("adb.exe", "adb"):
            candidate = Path(root) / "platform-tools" / executable
            if candidate.is_file():
                return str(candidate)
    raise LaunchError("ADB was not found. Install Android platform-tools or pass --adb PATH.")


def parse_devices(output: str) -> list[tuple[str, str]]:
    devices = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and not line.startswith(("List of devices", "*")):
            devices.append((parts[0], parts[1]))
    return devices


def choose_device(devices: list[tuple[str, str]], serial: str | None) -> str:
    if serial:
        status = dict(devices).get(serial)
        if status != "device":
            raise LaunchError(f"Device {serial} is {status or 'not connected'}. Check USB debugging and its authorization prompt.")
        return serial
    ready = [name for name, status in devices if status == "device"]
    if len(ready) == 1:
        return ready[0]
    if not ready:
        details = ", ".join(f"{name}: {status}" for name, status in devices)
        raise LaunchError("No usable ADB device. Connect the phone and allow USB debugging." + (f" ({details})" if details else ""))
    raise LaunchError("More than one device is connected. Select one using --serial: " + ", ".join(ready))


class Device:
    def __init__(self, adb: str, serial: str, su: str = "su"):
        self.prefix = [adb, "-s", serial]
        self.su = shlex.split(su)
        if not self.su:
            raise LaunchError("--su must contain an Android su executable.")

    def run(self, *args: str, timeout: float = 30) -> str:
        return command([*self.prefix, *args], timeout)

    def shell(self, script: str, root: bool = False, timeout: float = 30) -> str:
        # adb receives ONE shell command. shlex.quote applies to Android's shell,
        # not PowerShell or cmd.exe, neither of which is invoked here.
        if root:
            script = shlex.join([*self.su, "-c", script])
        return self.run("shell", script, timeout=timeout)

    def verify_root(self) -> None:
        uid = self.shell("id -u", root=True, timeout=45)
        if uid.strip() != "0":
            raise LaunchError(f"Root check returned {uid!r}. Allow ADB shell in KernelSU and try again.")

    def forward(self, port: int) -> None:
        local, remote = f"tcp:{port}", f"tcp:{DEVICE_PORT}"
        for line in self.run("forward", "--list").splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[1] == local:
                if fields == [self.prefix[2], local, remote]:
                    return
                raise LaunchError(f"Computer port {port} is already forwarded to another target. Choose a different --port.")
        self.run("forward", "--no-rebind", local, remote)

    def stop_owned_service(self) -> None:
        """Only stop a pid whose /proc executable matches our exact deployed file."""
        executable = shlex.quote(f"{REMOTE}/UnrealMemoryTools")
        pidfile = shlex.quote(f"{REMOTE}/service.pid")
        script = f"""set -e
pidfile={pidfile}
expected={executable}
if [ -f "$pidfile" ]; then
  old=$(cat "$pidfile")
  case "$old" in ''|*[!0-9]*) old=0;; esac
  if [ "$old" -gt 1 ] && [ "$(readlink /proc/$old/exe 2>/dev/null || true)" = "$expected" ]; then
    kill -TERM "$old"
    count=0
    while [ "$(readlink /proc/$old/exe 2>/dev/null || true)" = "$expected" ] && [ "$count" -lt 40 ]; do
      sleep 0.1
      count=$((count + 1))
    done
    if [ "$(readlink /proc/$old/exe 2>/dev/null || true)" = "$expected" ]; then kill -KILL "$old"; fi
  fi
  rm -f "$pidfile"
fi"""
        self.shell(script, root=True, timeout=12)

    def deploy(self, binary: Path, web: Path) -> None:
        self.shell(f"mkdir -p {shlex.quote(REMOTE + '/web')} && chown 2000:2000 {shlex.quote(REMOTE)} {shlex.quote(REMOTE + '/web')}", root=True)
        self.run("push", str(binary), f"{REMOTE}/UnrealMemoryTools.new", timeout=120)
        # Directory push semantics differ between ADB versions and whether the
        # destination exists. Explicit destinations also avoid nesting web/web.
        for name in ("index.html", "style.css", "app.js"):
            self.run("push", str(web / name), f"{REMOTE}/web/{name}", timeout=30)
        # Stage first so an incomplete push does not destroy the previous binary.
        self.stop_owned_service()
        self.shell(f"chmod 755 {shlex.quote(REMOTE + '/UnrealMemoryTools.new')} && "
                   f"mv {shlex.quote(REMOTE + '/UnrealMemoryTools.new')} {shlex.quote(REMOTE + '/UnrealMemoryTools')}", root=True)

    def start(self, native: bool = False) -> None:
        binary = f"{REMOTE}/UnrealMemoryTools"
        args = [binary, "--output", OUTPUT]
        if not native:
            args += ["--serve", "--port", str(DEVICE_PORT), "--web", f"{REMOTE}/web"]
        script = f"""set -e
test -x {shlex.quote(binary)}
test -f {shlex.quote(REMOTE + '/web/index.html')}
mkdir -p {shlex.quote(OUTPUT)}
nohup {shlex.join(args)} >{shlex.quote(REMOTE + '/service.log')} 2>&1 </dev/null &
echo $! >{shlex.quote(REMOTE + '/service.pid')}
"""
        self.shell(script, root=True, timeout=15)

    def verify_native(self) -> None:
        time.sleep(0.5)
        script = f"""pid=$(cat {shlex.quote(REMOTE + '/service.pid')})
case "$pid" in ''|*[!0-9]*) exit 1;; esac
test "$(readlink /proc/$pid/exe)" = {shlex.quote(REMOTE + '/UnrealMemoryTools')}"""
        try:
            self.shell(script, root=True)
        except LaunchError as exc:
            log = self.shell(f"tail -n 30 {shlex.quote(REMOTE + '/service.log')}", root=True)
            raise LaunchError(f"Native UI did not stay running. Use a build with UMT_NATIVE_UI enabled.\n{log}") from exc


def get_health(url: str, timeout: float = 2) -> dict:
    # A local forward must never go through HTTP_PROXY / corporate proxy settings.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url + "/api/health", timeout=timeout) as response:
        data = json.load(response)
    if not isinstance(data, dict) or "version" not in data or "uid" not in data:
        raise LaunchError("The forwarded port did not return a UMT health response.")
    return data


def wait_for_health(url: str, deadline_seconds: float = 20) -> dict:
    deadline = time.monotonic() + deadline_seconds
    error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return get_health(url)
        except (OSError, ValueError, urllib.error.URLError, LaunchError) as exc:
            error = exc
            time.sleep(0.3)
    raise LaunchError(f"The Android service did not become ready: {error}")


def local_json(url: str, timeout: float = 2) -> dict:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=timeout) as response:
        data = json.load(response)
    if not isinstance(data, dict):
        raise LaunchError("The local service returned an invalid status response.")
    return data


def port_in_use(port: int) -> bool:
    with socket.socket() as listener:
        try:
            listener.bind(("127.0.0.1", port))
            return False
        except OSError:
            return True


def host_process_identity(pid: int) -> dict | None:
    """Read executable and creation time, preventing pid reuse during cleanup."""
    if os.name != "nt":
        try:
            executable = str(Path(f"/proc/{pid}/exe").resolve(strict=True))
            stat = Path(f"/proc/{pid}/stat").read_text()
            birth = stat[stat.rfind(")") + 2:].split()[19]
            return {"exe": executable, "birth": birth}
        except (OSError, IndexError):
            return None
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        created, ended, system, user = (wintypes.FILETIME() for _ in range(4))
        if not kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)) or not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(ended), ctypes.byref(system), ctypes.byref(user)):
            return None
        return {"exe": str(Path(buffer.value).resolve()), "birth": str((created.dwHighDateTime << 32) | created.dwLowDateTime)}
    finally:
        kernel.CloseHandle(handle)


def stop_owned_host(pidfile: Path, script: Path, status: dict) -> None:
    try:
        recorded = json.loads(pidfile.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise LaunchError("A bridge is already using this port and has no matching launcher pidfile. Choose --workbench-port.") from exc
    pid = int(recorded.get("pid", 0))
    identity = host_process_identity(pid) if pid > 1 else None
    expected = str(script.resolve())
    if not identity or identity != recorded.get("identity") or recorded.get("script") != expected or int(status.get("pid", 0)) != pid:
        raise LaunchError("The running bridge does not match this launcher's recorded process. It was left running; choose --workbench-port.")
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x0001, False, pid)
        if not handle:
            raise LaunchError("Could not open the owned workbench process for shutdown.")
        try:
            if host_process_identity(pid) != identity or not kernel.TerminateProcess(handle, 0):
                raise LaunchError("Could not safely stop the previous owned workbench process.")
        finally:
            kernel.CloseHandle(handle)
    else:
        import signal
        os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 5
    while host_process_identity(pid) == identity and time.monotonic() < deadline:
        time.sleep(0.1)


def start_workbench(device_url: str, port: int, workspace: Path) -> str:
    script = PROJECT / "bridge/workbench.py"
    if not script.is_file():
        raise LaunchError(f"The desktop bridge is missing: {script}")
    workspace = workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    url = f"http://127.0.0.1:{port}"
    pidfile = workspace / f"launcher-workbench-{port}.json"
    if port_in_use(port):
        try:
            status = local_json(url + "/api/workbench/status")
        except (OSError, ValueError, urllib.error.URLError, LaunchError) as exc:
            raise LaunchError(f"Port {port} is in use by another service. Choose --workbench-port.") from exc
        if status.get("deviceUrl", "").rstrip("/") == device_url.rstrip("/") and Path(status.get("artifactRoot", "")).resolve() == workspace:
            return url
        stop_owned_host(pidfile, script, status)
    log_path = workspace / "workbench.log"
    argv = [sys.executable, str(script), "--device-url", device_url, "--port", str(port), "--workspace", str(workspace)]
    try:
        with log_path.open("ab") as log:
            process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       cwd=str(PROJECT), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                                       start_new_session=os.name != "nt")
    except OSError as exc:
        raise LaunchError(f"Could not start the desktop workbench: {exc}") from exc
    identity = host_process_identity(process.pid)
    pidfile.write_text(json.dumps({"pid": process.pid, "identity": identity, "script": str(script.resolve()), "deviceUrl": device_url}, indent=2), encoding="utf-8")
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        try:
            status = local_json(url + "/api/workbench/status")
            if "version" in status and status.get("deviceUrl", "").rstrip("/") == device_url.rstrip("/"):
                return url
        except (OSError, ValueError, urllib.error.URLError, LaunchError):
            pass
        time.sleep(0.2)
    # Popen itself proves this is our child even if startup failed before a
    # trustworthy pidfile identity was available.
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
    detail = log_path.read_text(encoding="utf-8", errors="replace")[-4000:]
    raise LaunchError(f"The desktop workbench did not become ready.\n{detail}")


def port_number(value: str) -> int:
    try:
        port = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("port must be an integer") from exc
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ADB serial (required when multiple devices are connected)")
    parser.add_argument("--adb", help="Path to adb or adb.exe")
    parser.add_argument("--su", default="su", help="Android root command, e.g. su or /data/adb/ksu/bin/su")
    parser.add_argument("--port", type=port_number, default=18765, help="Computer loopback port (default: 18765)")
    parser.add_argument("--binary", type=Path, default=PROJECT / "outputs/arm64-v8a/UnrealMemoryTools")
    parser.add_argument("--no-open", action="store_true", help="Do not open a browser")
    parser.add_argument("--no-deploy", action="store_true", help="Reuse the already deployed Android binary and UI")
    parser.add_argument("--native", action="store_true", help="Start the standalone phone overlay instead of the HTTP interface")
    parser.add_argument("--workbench-port", type=port_number, default=18766, help="Computer analysis workbench port (default: 18766)")
    parser.add_argument("--workspace", type=Path, default=PROJECT / "artifacts", help="Computer dump and IDA artifact directory")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if not args.native and args.workbench_port == args.port:
            raise LaunchError("--workbench-port must differ from the ADB forwarding --port.")
        web = Path(__file__).resolve().parent / "web"
        if not args.no_deploy:
            if not args.binary.is_file():
                raise LaunchError(f"Build the Android binary first; it was not found:\n{args.binary}")
            if not all((web / name).is_file() for name in ("index.html", "style.css", "app.js")):
                raise LaunchError(f"Web assets are missing: {web}")
        adb = find_adb(args.adb)
        serial = choose_device(parse_devices(command([adb, "devices", "-l"])), args.serial)
        device = Device(adb, serial, args.su)
        print(f"Device: {serial}\nChecking root (allow shell in KernelSU if prompted)…", flush=True)
        device.verify_root()
        if not args.no_deploy:
            print("Deploying native service and web interface…", flush=True)
            device.deploy(args.binary.resolve(), web)
        if args.native:
            device.stop_owned_service()
            device.start(native=True)
            device.verify_native()
            print(f"\nStandalone phone UI started. Check the phone screen.\nExports: {OUTPUT}")
            print("The HTTP interface is stopped; this mode uses one native process.")
            return 0
        device.forward(args.port)
        url = f"http://127.0.0.1:{args.port}"
        health = None
        if args.no_deploy:
            try:
                health = get_health(url)
            except (OSError, ValueError, urllib.error.URLError, LaunchError):
                pass
        if health is None:
            device.stop_owned_service()
            device.start()
            try:
                health = wait_for_health(url)
            except LaunchError as exc:
                log = device.shell(f"tail -n 30 {shlex.quote(REMOTE + '/service.log')}", root=True)
                raise LaunchError(f"{exc}\nAndroid service log:\n{log}") from exc
        if str(health["uid"]) != "0":
            raise LaunchError("The running Android service has no root access. Allow shell in KernelSU and redeploy.")
        desktop_url = start_workbench(url, args.workbench_port, args.workspace)
        print(f"\nUMT {health['version']} is ready (root, PID {health.get('pid', '?')}).")
        print(f"Computer: {desktop_url}\nDevice forwarding: {url}\nPhone:    http://127.0.0.1:{DEVICE_PORT}")
        print(f"Exports:  {OUTPUT}\nLog:      {REMOTE}/service.log")
        print("The phone service stays running after this launcher exits. Keep ADB connected for the computer UI.")
        if not args.no_open:
            webbrowser.open(desktop_url)
        return 0
    except (LaunchError, KeyboardInterrupt) as exc:
        print(f"\nLaunch failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    raise SystemExit(main())
