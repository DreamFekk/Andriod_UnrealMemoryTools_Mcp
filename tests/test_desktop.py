"""No-device launcher tests; optional real-browser tests use an isolated fake API.

python -m unittest discover -s tests -p test_desktop.py -v
$env:UMT_BROWSER_TEST='1' enables Playwright + installed Edge for UI verification.
"""
from __future__ import annotations

from contextlib import contextmanager
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import threading
import time
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("umt_launcher", ROOT / "desktop/launcher.py")
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

BASE_STATE = {
    "busy": False, "phase": "", "error": "", "package": "", "pid": 0,
    "profile": "", "probed": False, "objectsPercent": 0, "dumpPercent": 0,
    "resultPath": "", "soDumpPath": "", "logs": [], "offsets": [], "groups": [], "metrics": {}, "generation": 1,
}


@contextmanager
def fake_server():
    state = deepcopy(BASE_STATE)
    calls = []
    files = []
    jobs = {}
    workbench_calls = []
    sessions = [{"id": "fixture-session", "name": "Peace Elite · device dump", "package": "com.tencent.tmgp.pubgmhd"}]

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, value, status=200):
            data = json.dumps(value, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/api/health":
                self.send({"version": "UI TEST FIXTURE", "uid": 0, "pid": 42})
            elif path == "/api/state":
                self.send(state)
            elif path == "/api/processes":
                self.send([{"pid": 22480, "package": "com.tencent.tmgp.pubgmhd", "profileName": "PUBGM CN / 64-bit", "dedicated": True}])
            elif path == "/api/files":
                self.send(files)
            elif path == "/api/workbench/status":
                self.send({"version": "fixture", "pid": 42, "deviceUrl": "http://127.0.0.1:18765", "idaUrl": "http://127.0.0.1:13337", "artifactRoot": "D:/UMT/artifacts", "ida": {"connected": True, "tools": ["decompile", "xrefs"]}, "sessions": sessions, "jobs": list(jobs.values()), "commands": workbench_calls,
                           "mcpConfig": {"mcpServers": {"unreal-memory-tools": {"command": "python", "args": ["bridge/mcp_server.py"]}}}})
            elif path == "/api/workbench/job":
                identifier = parse_qs(urlsplit(self.path).query)["id"][0]
                self.send(jobs[identifier])
            elif path == "/api/workbench/report":
                self.send({"report": "fixture report"})
            elif path.startswith("/download/"):
                self.send({"fixture": True})
            elif path in ("/", "/app.js", "/style.css"):
                name = "index.html" if path == "/" else path[1:]
                data = (ROOT / "desktop/web" / name).read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", {"index.html": "text/html", "app.js": "text/javascript", "style.css": "text/css"}[name] + "; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send({}, 404)

        def do_POST(self):
            if self.headers.get("X-UMT-Client") != "1":
                self.send({"error": "Missing client header"}, 403)
                return
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/api/workbench/command":
                workbench_calls.append(body)
                command = body["command"]
                if command == "search_functions":
                    self.send({"functions": [{"name": "APlayerPawn::GetHealth", "rva": "0xF123456789ABCDEF", "signature": "float APlayerPawn::GetHealth() const", "session": "fixture-session"}]})
                elif command == "ida_status":
                    self.send({"connected": True, "tools": ["decompile", "xrefs"]})
                else:
                    identifier = f"job-{len(jobs)}"
                    result = {"session": "fixture-session", "rva": body.get("rva", "0x1234"), "evidence": {
                        "decompile": {"result": {"content": [{"type": "text", "text": json.dumps({"addr": "0x1234", "code": "float __fastcall GetHealth(APlayerPawn *self) { return self->Health; }"})}]}},
                        "xrefs": {"result": {"structuredContent": {"result": [{"xrefs": [{"address": "0xF123456789ABC000", "name": "UpdateHud", "type": "call"}]}]}}}}}
                    jobs[identifier] = {"id": identifier, "status": "done", "result": result}
                    self.send({"jobId": identifier, "status": "running"})
                return
            if self.path == "/api/explorer":
                calls.append({"endpoint": "explorer", **body})
                if state["busy"] or not state["probed"]:
                    self.send({"error": "Probe the target first / task busy", "busy": state["busy"]}, 409)
                    return
                if body['action']=='worlds':
                    cursor=body.get('cursor',0)
                    candidates=([{'address':'0xF123456780000001','name':'SublevelWorld','hasGameInstance':False,'levelCount':0}]
                                if cursor==0 else [{'address':'0xF123456780000002','name':'ActiveWorld','hasGameInstance':True,'levelCount':2}])
                    self.send({'generation':state['generation'],'items':candidates,'nextCursor':100 if cursor==0 else 200,'total':200,'scanned':100,'complete':cursor>0})
                    return
                if body['action']=='actors':
                    topology=str(state.get('fixtureTopology','one'))
                    if body.get('expectedTopology') and body['expectedTopology']!=topology:
                        self.send({'error':'Level/Actors arrays changed','changed':True},409);return
                    count=state.get('fixtureActorCount',130)
                    levels=[{'address':'0xF123456780000100','name':'PersistentLevel','count':0,'valid':True},
                            {'address':'0xF123456780000200','name':'LoadedSublevel','count':count,'valid':True}]
                    if body.get('levelAddress')==levels[0]['address']:count=0
                    cursor=body.get('cursor',0);limit=body.get('limit',100)
                    end=min(count,cursor+max(1,min(limit,40 if body.get('query') else limit)))
                    items=[{'address':f'0xF123456789AB{i:04X}','name':f'PlayerPawn_{i}','fullName':f'Actor World.PlayerPawn_{i}',
                            'className':'PlayerPawn','levelAddress':levels[1]['address'],'levelName':'LoadedSublevel',
                            'actorSlot':i,'slotAddress':hex(0xf123456780001000+i*8),'index':i} for i in range(cursor,end)]
                    query=body.get('query','').lower()
                    items=[item for item in items if query in item['fullName'].lower()]
                    self.send({'generation':state['generation'],'world':{'address':body['worldAddress']},
                               'levels':levels,'actorsOffset':'0x98','topology':topology,'items':items,
                               'totalSlots':count,'nextCursor':end,'scanned':end-cursor,'invalidSlots':0,
                               'complete':end>=count,'warnings':[],'sampledAtMs':int(time.time()*1000)})
                    return
                if body["action"] == "list":
                    query = body.get("query", "").lower()
                    if query == "fixtureslow":
                        time.sleep(0.15)
                    start = int(body.get("cursor", 0))
                    limit = int(body.get("limit", 100))
                    # The filtered fixture takes several bounded requests,
                    # exercising real continuation rather than local filtering.
                    end = min(260, start + (40 if query else limit))
                    objects = [{"index": index, "address": f"0xF123456789AB{index:04X}",
                                "name": f"PlayerPawn_{index}", "fullName": f"BP_PlayerPawn_C World.PersistentLevel.PlayerPawn_{index}",
                                "className": "BP_PlayerPawn_C"} for index in range(start, end)]
                    objects = [item for item in objects if query in item["fullName"].lower()][:limit]
                    self.send({"generation": state["generation"], "total": 260, "items": objects,
                               "nextCursor": end, "scanned": end - start, "complete": end >= 260})
                    return
                if body["action"] == "inspect":
                    address = body["address"]
                    if address=='0xF123456789AB0600':
                        fields=[]
                        for i,(name,typename,offset) in enumerate((('Pawn','APawn*','0x5D8'),('Character','ACharacter*','0x5E8'),('PlayerState','APlayerState*','0x5F0'))):
                            fields.append({'name':name,'type':typename,'owner':'Controller','offset':offset,
                                           'address':hex(0xf123456789ad0000+i*0x100),'descriptorAddress':hex(0xf123456789ad0000+i*0x100),
                                           'value':'<field descriptor>','valueKind':'declaration',
                                           'typeLinks':[{'name':typename.rstrip('*'),'address':hex(0xf123456789ab0700+i),'kind':'object'}]})
                        self.send({'generation':state['generation'],'address':address,'name':'Controller','className':'Class',
                                   'fullName':'Class Engine.Controller','isInstance':False,'metadata':[],'properties':fields,
                                   'functions':[],'containers':[],'runtime':{}})
                        return
                    if state.get("fixtureExpired") and body.get("expectedIdentity"):
                        self.send({"error": "Instance identity changed"}, 409)
                        return
                    if address.endswith("DEAD"):
                        self.send({"error": "Object memory is unavailable."}, 409)
                        return
                    if body.get('kind') == 'struct':
                        if not body.get('typeAddress'):
                            self.send({'error': 'Missing typeAddress'}, 400)
                            return
                        target = '0xF123456789AC0700' if body.get('dereference') else address
                        self.send({'generation': state['generation'], 'kind': 'struct', 'address': target,
                                   'sourceAddress': address, 'typeAddress': body['typeAddress'], 'dereferenced': bool(body.get('dereference')),
                                   'identity': 'struct:'+target, 'name': 'Vector', 'className': 'FVector', 'fullName': 'FVector @ '+target,
                                   'metadata': [], 'properties': [{'name': 'X', 'type': 'float', 'owner': 'Vector', 'offset': '0x0', 'address': target, 'value': '12.5'},
                                                                  {'name': 'Y', 'type': 'float', 'owner': 'Vector', 'offset': '0x4', 'address': hex(int(target,16)+4), 'value': '-2.25'}],
                                   'functions': [], 'containers': [], 'runtime': {}})
                        return
                    index = int(address[-4:], 16)
                    name = f"PlayerPawn_{index}" if body.get("kind") != "field" else "HealthProperty"
                    self.send({"generation": state["generation"], "address": address, "index": index, "name": name, "identity": "fixture:"+address,
                               "fullName": f"BP_PlayerPawn_C World.PersistentLevel.{name}", "className": "BP_PlayerPawn_C",
                               "classAddress": "0xF123456789AB0200", "outerAddress": "0xF123456789AB0300", "superAddress": "0xF123456789AB0400",
                               "metadata": [{"name": "InternalIndex", "value": str(index)}, {"name": "ObjectFlags", "value": "0x8"}],
                               "properties": [{"name": "Health", "type": "float", "offset": "0x318", "address": "0xF123456789AC0318", "value": state.get('fixtureHealth','100.0')},
                                              {"name": "Location", "type": "FVector", "owner": "Actor", "offset": "0x700", "address": "0xF123456789AC0700", "value": "X=12.5 Y=-2.25 Z=0", "valueKind": "vector3", "scalarType": "float32", "structAddress": "0xF123456789AD0100"},
                                              {"name": "Controller", "type": "APlayerController*", "offset": "0x420", "address": "0xF123456789AC0420", "value": "PlayerController_2", "pointer": "0xF123456789AB0002"}],
                               "functions": [{"name": "GetHealth", "address": "0xF123456780001234", "rva": "0x1234", "flags": "0x04020401", "numParams": 1, "paramSize": 4, "signature": "float GetHealth() const"}],
                               "containers": [{"name": "Inventory", "type": "TArray<UObject*>", "kind": "array", "address": "0xF123456789AC0500", "dataAddress": "0xF123456789AC0600", "count": 1, "max": 8, "elementSize": 8,
                                               "items": [{"index": 0, "address": "0xF123456789AC0600", "type": "UObject*", "value": "Weapon_4", "pointer": "0xF123456789AB0004"}]}],
                               "runtime": {"kind": "AActor", "fields": [{"name": "RootComponent", "address": "0xF123456789AC0800", "value": "SceneComponent", "pointer": "0xF123456789AB0005"}],
                                           "actors": [{"index": 0, "address": "0xF123456789AB0006", "name": "Actor_6", "fullName": "Actor World.Actor_6"}], "actorCount": 1, "actorMax": 8}})
                    return
                self.send({"error": "Unknown explorer action"}, 400)
                return
            calls.append(body)
            action = body["action"]
            if action == "probe":
                state.update(pid=body["pid"], package="com.tencent.tmgp.pubgmhd", profile="PUBGM CN", probed=True,
                             phase="探测完成 · 9 / 9 关键地址已定位", objectsPercent=100,
                             metrics={"elapsedMs": 1830, "objectsCount": 182640, "bytesRead": 73400320, "cacheHits": 8420},
                             offsets=[{"name": name, "value": f"0x72{index:08X}", "relative": f"0x{index:08X}", "found": True}
                                      for index, name in enumerate(["GNames", "GObjects", "GWorld", "ProcessEvent"], 1)],
                             groups=[{"name": "UObject", "fields": [{"name": "ClassPrivate", "type": "UClass*", "offset": "0x10", "found": True, "description": "对象类型"},
                                                                      {"name": "NamePrivate", "type": "FName", "offset": "0x18", "found": True, "description": "对象名称"}]}],
                             logs=["[21:32:08] Attach: com.tencent.tmgp.pubgmhd (22480)", "[21:32:08] Engine: libUE4.so / arm64-v8a", "[21:32:09] Objects: 182,640 · cached reads enabled", "[21:32:09] Probe completed in 1.83s", "<img src=x onerror=alert(1)>"])
            elif action in ("dump", "library"):
                state.update(dumpPercent=100, phase="导出完成", resultPath="/sdcard/UnrealMemoryTools_Refactored/SDK.txt")
                files[:] = [{"name": "SDK.txt", "size": 24719360, "url": "/download/SDK.txt"},
                            {"name": "Objects.txt", "size": 9583152, "url": "/download/Objects.txt"},
                            {"name": "Offsets.json", "size": 1824, "url": "/download/Offsets.json"}]
            elif action == "clear_logs":
                state["logs"] = []
            self.send({"accepted": True})

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", state, calls, files
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


class LauncherTests(unittest.TestCase):
    def test_host_process_identity_and_port_probe(self):
        identity = launcher.host_process_identity(os.getpid())
        self.assertEqual(Path(identity["exe"]).resolve(), Path(os.sys.executable).resolve())
        self.assertTrue(identity["birth"])
        with fake_server() as (url, *_):
            self.assertTrue(launcher.port_in_use(urlsplit(url).port))

    def test_reuse_matching_host_and_reject_unowned_stop(self):
        script = ROOT / "bridge/workbench.py"
        with patch.object(Path, "is_file", return_value=True), patch.object(launcher, "port_in_use", return_value=True), patch.object(launcher, "local_json", return_value={"version": "fixture", "deviceUrl": "http://127.0.0.1:18765", "artifactRoot": str(ROOT / "artifacts")}), patch.object(subprocess, "Popen") as spawn:
            url = launcher.start_workbench("http://127.0.0.1:18765", 18766, ROOT / "artifacts")
            self.assertEqual(url, "http://127.0.0.1:18766")
            spawn.assert_not_called()
        with self.assertRaisesRegex(launcher.LaunchError, "no matching launcher pidfile"):
            launcher.stop_owned_host(ROOT / "does-not-exist.pid", script, {"pid": 1234})

    def test_host_launch_is_hidden_and_records_exact_identity(self):
        from unittest.mock import MagicMock
        process = MagicMock(pid=4321)
        process.poll.return_value = None
        identity = {"exe": str(Path(os.sys.executable).resolve()), "birth": "123456789"}
        with tempfile.TemporaryDirectory() as directory, patch.object(Path, "is_file", return_value=True), patch.object(launcher, "port_in_use", return_value=False), patch.object(launcher, "host_process_identity", return_value=identity), patch.object(launcher, "local_json", return_value={"version": "fixture", "deviceUrl": "http://127.0.0.1:18765", "pid": 4321}), patch.object(subprocess, "Popen", return_value=process) as spawn:
            workspace = Path(directory)
            self.assertEqual(launcher.start_workbench("http://127.0.0.1:18765", 18766, workspace), "http://127.0.0.1:18766")
            arguments = spawn.call_args.args[0]
            self.assertEqual(arguments[:2], [os.sys.executable, str(ROOT / "bridge/workbench.py")])
            self.assertIn("--workspace", arguments)
            self.assertEqual(spawn.call_args.kwargs["creationflags"], getattr(subprocess, "CREATE_NO_WINDOW", 0))
            record = json.loads((workspace / "launcher-workbench-18766.json").read_text(encoding="utf-8"))
            self.assertEqual(record["identity"], identity)
            self.assertEqual(record["pid"], 4321)

    def test_host_pid_reuse_is_not_terminated(self):
        with tempfile.TemporaryDirectory() as directory:
            pidfile = Path(directory) / "service.json"
            script = ROOT / "bridge/workbench.py"
            pidfile.write_text(json.dumps({"pid": 1234, "identity": {"exe": "python", "birth": "old"}, "script": str(script.resolve())}), encoding="utf-8")
            with patch.object(launcher, "host_process_identity", return_value={"exe": "python", "birth": "new"}):
                with self.assertRaisesRegex(launcher.LaunchError, "does not match"):
                    launcher.stop_owned_host(pidfile, script, {"pid": 1234})

    def test_parse_and_select_device(self):
        output = "* daemon started successfully *\nList of devices attached\nA\tdevice product:p model:m\nB unauthorized\nC offline\n"
        devices = launcher.parse_devices(output)
        self.assertEqual(devices, [("A", "device"), ("B", "unauthorized"), ("C", "offline")])
        self.assertEqual(launcher.choose_device(devices, None), "A")
        with self.assertRaisesRegex(launcher.LaunchError, "unauthorized"):
            launcher.choose_device(devices, "B")
        with self.assertRaisesRegex(launcher.LaunchError, "More than one"):
            launcher.choose_device([("A", "device"), ("B", "device")], None)

    def test_shell_root_quoting_stays_one_adb_argument(self):
        script = "printf '%s' 'path with spaces; $(not-executed-by-host)'"
        with patch.object(launcher, "command", return_value="0") as run:
            launcher.Device("C:/ADB Tools/adb.exe", "serial:5555", "/data/adb/ksu/bin/su").shell(script, root=True)
        args = run.call_args.args[0]
        self.assertEqual(args[:4], ["C:/ADB Tools/adb.exe", "-s", "serial:5555", "shell"])
        self.assertEqual(len(args), 5)
        self.assertEqual(shlex.split(args[4]), ["/data/adb/ksu/bin/su", "-c", script])

    def test_only_owned_service_can_be_stopped(self):
        device = launcher.Device("adb", "A")
        with patch.object(device, "shell") as shell:
            device.stop_owned_service()
        script = shell.call_args.args[0]
        self.assertIn(f"expected={launcher.REMOTE}/UnrealMemoryTools", script)
        self.assertGreaterEqual(script.count("readlink /proc/$old/exe"), 3)
        self.assertNotIn("killall", script)
        self.assertNotIn("pkill", script)

    def test_failed_commands_and_timeout_report_errors(self):
        with patch.object(subprocess, "run", return_value=subprocess.CompletedProcess(["adb"], 1, "", "permission denied")):
            with self.assertRaisesRegex(launcher.LaunchError, "permission denied"):
                launcher.command(["adb"])
        with patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired(["adb"], 5)):
            with self.assertRaisesRegex(launcher.LaunchError, "timed out"):
                launcher.command(["adb"], timeout=5)

    def test_root_check_rejects_non_root_uid(self):
        device = launcher.Device("adb", "A")
        with patch.object(device, "shell", return_value="2000"):
            with self.assertRaisesRegex(launcher.LaunchError, "KernelSU"):
                device.verify_root()

    def test_forward_keeps_unrelated_mappings(self):
        device = launcher.Device("adb", "A")
        with patch.object(device, "run", return_value="B tcp:18765 tcp:1234") as run:
            with self.assertRaisesRegex(launcher.LaunchError, "another target"):
                device.forward(18765)
            self.assertEqual(run.call_count, 1)
        with patch.object(device, "run", return_value="A tcp:18765 tcp:18765") as run:
            device.forward(18765)
            self.assertEqual(run.call_count, 1)
        with patch.object(device, "run", return_value="") as run:
            device.forward(18766)
            run.assert_called_with("forward", "--no-rebind", "tcp:18766", "tcp:18765")

    def test_native_mode_uses_same_owned_pidfile_without_http(self):
        device = launcher.Device("adb", "A")
        with patch.object(device, "shell") as shell:
            device.start(native=True)
        script = shell.call_args.args[0]
        self.assertNotIn("--serve", script)
        self.assertIn("service.pid", script)
        self.assertIn("--output", script)

    def test_deploy_pushes_assets_to_exact_paths(self):
        device = launcher.Device("adb", "A")
        web = ROOT / "desktop/web"
        with patch.object(device, "shell"), patch.object(device, "stop_owned_service"), patch.object(device, "run") as run:
            device.deploy(Path("binary"), web)
        pushes = [call.args for call in run.call_args_list]
        self.assertEqual(len(pushes), 4)
        for name in ("index.html", "style.css", "app.js"):
            self.assertIn(("push", str(web / name), f"{launcher.REMOTE}/web/{name}"), pushes)

    def test_health_is_validated_through_local_http(self):
        with fake_server() as (url, *_):
            health = launcher.wait_for_health(url, 2)
        self.assertEqual(health["version"], "UI TEST FIXTURE")
        self.assertEqual(health["uid"], 0)

    def test_port_validation(self):
        self.assertEqual(launcher.port_number("18765"), 18765)
        for value in ("0", "65536", "abc"):
            with self.assertRaises(Exception):
                launcher.port_number(value)


@unittest.skipUnless(os.environ.get("UMT_BROWSER_TEST") == "1", "Set UMT_BROWSER_TEST=1 for Playwright UI checks")
class BrowserTests(unittest.TestCase):
    def test_workbench_auto_recovers_and_preserves_selection_after_disconnect(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url, *_), sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel=os.environ.get('UMT_BROWSER', 'msedge'), headless=True)
            page = browser.new_page()
            unavailable = [True]
            def status_route(route):
                if unavailable[0]:
                    route.fulfill(status=503, content_type='application/json', body=json.dumps({'error': 'fixture bridge restarting'}))
                else:
                    route.continue_()
            page.route('**/api/workbench/status', status_route)
            page.goto(url)
            expect(page.locator('#workbench-connection-message')).to_contain_text('每 4 秒')
            unavailable[0] = False
            expect(page.locator('#workbench-content')).to_be_visible(timeout=10000)
            page.locator('#function-query').fill('GetHealth')
            page.locator('#find-functions').click()
            expect(page.locator('.function-choice')).to_have_count(1)
            page.locator('.function-choice').click()
            selected = page.locator('#selected-function-name').inner_text()
            unavailable[0] = True
            page.locator('#workbench-refresh').click()
            expect(page.locator('#workbench-unavailable')).to_be_visible()
            expect(page.locator('#workbench-content')).to_be_visible()
            expect(page.locator('#selected-function-name')).to_have_text(selected)
            expect(page.locator('#analyze-function')).to_be_disabled()
            unavailable[0] = False
            expect(page.locator('#workbench-unavailable')).to_be_hidden(timeout=10000)
            expect(page.locator('#analyze-function')).to_be_enabled()
            expect(page.locator('#selected-function-name')).to_have_text(selected)
            browser.close()

    def test_phone_endpoint_explains_missing_workbench_and_links_computer(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url, *_), sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel=os.environ.get('UMT_BROWSER', 'msedge'), headless=True)
            page = browser.new_page(viewport={'width': 390, 'height': 844})
            page.route('**/api/workbench/status', lambda route: route.fulfill(status=404, content_type='application/json', body=json.dumps({'error': 'Not found'})))
            page.goto(url)
            expect(page.locator('#workbench-connection-message')).to_contain_text('没有电脑端 IDA/MCP 接口')
            expect(page.locator('#open-desktop-workbench')).to_have_attribute('href', 'http://127.0.0.1:18766/#workbench')
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
            browser.close()

    def test_world_actor_traversal_search_levels_and_inspector(self):
        from playwright.sync_api import sync_playwright,expect
        with fake_server() as (url,state,calls,_),sync_playwright() as p:
            state.update(probed=True,pid=22480,package='com.tencent.tmgp.pubgmhd')
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page(viewport={'width':1440,'height':1080})
            errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
            page.goto(url);page.locator('#tab-world').click()
            expect(page.locator('.world-actor')).to_have_count(100,timeout=10000)
            expect(page.locator('#world-select')).to_have_value('0xF123456780000002')
            page.locator('.world-actor').first.click()
            expect(page.locator('#world-inspector-host #object-title')).to_have_text('PlayerPawn_0')
            page.locator('#world-next').click()
            expect(page.locator('.world-actor')).to_have_count(30)
            expect(page.locator('.world-actor').first).to_contain_text('PlayerPawn_100')
            page.locator('#world-prev').click()
            expect(page.locator('.world-actor')).to_have_count(100)
            page.locator('#world-query').fill('PlayerPawn_129')
            page.locator('#world-search-form button').click()
            expect(page.locator('.world-actor')).to_have_count(1)
            expect(page.locator('.world-actor')).to_contain_text('PlayerPawn_129')
            page.locator('#world-query').fill('')
            page.locator('#world-level').select_option('0xF123456780000100')
            expect(page.locator('.world-actor')).to_have_count(0)
            page.locator('#world-level').select_option('')
            expect(page.locator('.world-actor')).to_have_count(100)
            page.locator('#world-next').click()
            expect(page.locator('.world-actor')).to_have_count(30)
            state['fixtureTopology']='two';state['fixtureActorCount']=5
            page.locator('#world-interval').select_option('1000');page.locator('#world-live').check()
            expect(page.locator('.world-actor')).to_have_count(5,timeout=8000)
            expect(page.locator('#world-page')).to_contain_text('1 ·')
            page.locator('#world-live').uncheck()
            page.set_viewport_size({'width':390,'height':844})
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
            self.assertTrue(any(c.get('action')=='actors' and c.get('cursor',0)>0 and c.get('expectedTopology') for c in calls))
            self.assertEqual(errors,[])
            browser.close()

    def test_world_generation_reset_discards_old_world_and_actor_pages(self):
        from playwright.sync_api import sync_playwright,expect
        with fake_server() as (url,state,calls,_),sync_playwright() as p:
            state.update(probed=True,pid=22480,package='com.tencent.tmgp.pubgmhd')
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page();page.goto(url);page.locator('#tab-world').click()
            expect(page.locator('.world-actor')).to_have_count(100,timeout=10000)
            page.locator('#world-live').check()
            state['generation']=2;state['fixtureActorCount']=3
            expect(page.locator('.world-actor')).to_have_count(3,timeout=10000)
            expect(page.locator('#world-live')).not_to_be_checked()
            self.assertTrue(any(c.get('action')=='worlds' and c.get('generation')==2 for c in calls))
            browser.close()

    def test_class_pointer_types_and_member_names_open_target_layout(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url,state,calls,_),sync_playwright() as p:
            state.update(probed=True,pid=22480,package='com.tencent.tmgp.pubgmhd')
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page()
            page.goto(url);page.locator('#tab-explorer').click()
            page.locator('#inspect-address').fill('0xF123456789AB0600')
            page.locator('#inspect-go').click()
            expect(page.locator('#object-title')).to_have_text('Controller')
            for i,name in enumerate(('APawn*','ACharacter*','APlayerState*')):
                link=page.locator('.type-link').filter(has_text=name)
                expect(link).to_have_count(1)
                link.click()
                expect(page.locator('#inspect-address')).to_have_value(hex(0xf123456789ab0700+i))
                page.locator('#inspect-back').click()
                expect(page.locator('#object-title')).to_have_text('Controller')
            row=page.locator('details[data-section="properties"] tr').filter(has=page.locator('.type-link').filter(has_text='APawn*'))
            row.locator('button').filter(has_text='Pawn').first.click()
            expect(page.locator('#inspect-address')).to_have_value('0xf123456789ab0700')
            inspected=[c['address'] for c in calls if c.get('action')=='inspect']
            self.assertNotIn('0xf123456789ad0000',inspected)
            browser.close()

    def test_instance_live_values_guard_busy_and_expiration(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url, state, calls, _), sync_playwright() as p:
            state.update(probed=True,pid=22480,package='com.tencent.tmgp.pubgmhd')
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page()
            page.goto(url)
            page.locator('#tab-explorer').click()
            page.locator('#inspect-kind').select_option('instance')
            page.locator('#inspect-address').fill('0xF123456789AB0001')
            page.locator('#inspect-go').click()
            expect(page.locator('#object-title')).to_have_text('PlayerPawn_1')
            expect(page.locator('#object-sections')).to_contain_text('X=12.5 Y=-2.25 Z=0')
            page.locator('#inspect-interval').select_option('500')
            page.locator('#inspect-live').check()
            state['fixtureHealth']='72.125'
            expect(page.locator('.value-changed')).to_contain_text('72.125',timeout=5000)
            inspections=[c for c in calls if c.get('action')=='inspect']
            self.assertTrue(any(c.get('expectedIdentity')=='fixture:0xF123456789AB0001' and c.get('generation')==1 for c in inspections))
            state['busy']=True
            page.wait_for_timeout(1800)
            count=len([c for c in calls if c.get('action')=='inspect'])
            page.wait_for_timeout(1000)
            self.assertEqual(count,len([c for c in calls if c.get('action')=='inspect']))
            state['busy']=False
            state['fixtureExpired']=True
            expect(page.locator('#inspect-live')).not_to_be_checked(timeout=5000)
            expect(page.locator('#explorer-error')).to_contain_text('identity changed')
            browser.close()

    def test_struct_navigation_back_and_explicit_pointer_slot(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url, state, calls, _), sync_playwright() as p:
            state.update(probed=True,pid=22480,package='com.tencent.tmgp.pubgmhd')
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page(viewport={'width':390,'height':844})
            page.goto(url)
            page.locator('#tab-explorer').click()
            page.locator('.object-select').first.click()
            page.locator('.struct-open').first.click()
            expect(page.locator('#inspect-kind')).to_have_value('struct')
            expect(page.locator('#inspect-type-address')).to_have_value('0xF123456789AD0100')
            expect(page.locator('#object-title')).to_have_text('Vector')
            expect(page.locator('#object-sections')).to_contain_text('-2.25')
            page.locator('#inspect-back').click()
            expect(page.locator('#object-title')).to_have_text('PlayerPawn_0')
            page.locator('#inspect-kind').select_option('struct')
            page.locator('#inspect-type-address').fill('0xF123456789AD0100')
            page.locator('#inspect-address').fill('0xF123456789AB0100')
            page.locator('#inspect-dereference').check()
            page.locator('#inspect-go').click()
            expect(page.locator('#object-title')).to_have_text('Vector')
            expect(page.locator('#inspect-address')).to_have_value('0xF123456789AB0100')
            self.assertTrue(any(c.get('dereference') and c.get('typeAddress')=='0xF123456789AD0100' for c in calls))
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
            browser.close()

    def test_ida_connected_without_open_capability_and_endpoint_change(self):
        from playwright.sync_api import sync_playwright,expect
        with fake_server() as (url,*_),sync_playwright() as p:
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page()
            endpoint={'url':'http://127.0.0.1:13337/mcp','open':False,'ready':False}
            commands=[]
            def status_route(route):
                route.fulfill(content_type='application/json',body=json.dumps({'version':'fixture','sessions':[{'id':'fixture-session'}],
                    'jobs':[],'idaUrl':endpoint['url'],'ida':{'connected':True,'canOpenDatabase':endpoint['open'],
                    'message':'' if endpoint['open'] else 'GUI endpoint lacks idb_open'}}))
            def command_route(route):
                body=route.request.post_data_json
                commands.append(body)
                if body.get('command')=='start_ida':
                    endpoint['url']=body['url']
                    route.fulfill(content_type='application/json',body=json.dumps({'jobId':'ida-start','status':'queued'}))
                else:route.continue_()
            def job_route(route):
                if not endpoint['ready']:
                    result={'id':'ida-start','kind':'start_ida','status':'running'}
                else:
                    endpoint['open']=True
                    result={'id':'ida-start','kind':'start_ida','status':'done','result':{
                        'ida':{'connected':True,'canOpenDatabase':True},'idaUrl':endpoint['url'],
                        'service':{'started':True,'reused':False,'pid':123,'message':'IDA MCP started successfully.'}}}
                route.fulfill(content_type='application/json',body=json.dumps(result))
            page.route('**/api/workbench/status',status_route)
            page.route('**/api/workbench/command',command_route)
            page.route('**/api/workbench/job?id=ida-start',job_route)
            page.goto(url)
            expect(page.locator('#ida-status')).to_contain_text('仅当前数据库')
            expect(page.locator('#ida-endpoint')).to_have_value('http://127.0.0.1:13337/mcp')
            expect(page.locator('#open-ida')).to_be_disabled()
            expect(page.locator('#ida-capability-message')).to_contain_text('idb_open')
            page.locator('#ida-endpoint').fill('http://127.0.0.1:13338/mcp')
            page.locator('#ida-endpoint-save').click()
            expect(page.locator('#ida-endpoint-save')).to_have_text('正在启动…')
            expect(page.locator('#ida-endpoint-save')).to_be_disabled()
            expect(page.locator('#connect-ida')).to_be_disabled()
            expect(page.locator('#ida-endpoint')).to_be_disabled()
            page.locator('#ida-endpoint-form').dispatch_event('submit')
            page.locator('#language').click()
            expect(page.locator('#ida-endpoint-save')).to_have_text('Starting…')
            self.assertEqual(len(commands),1)
            self.assertEqual(commands[0]['command'],'start_ida')
            endpoint['ready']=True
            expect(page.locator('#ida-status')).to_contain_text('database manager')
            expect(page.locator('#ida-endpoint-save')).to_have_text('Save & auto-connect')
            expect(page.locator('#ida-endpoint-save')).to_be_enabled()
            expect(page.locator('#ida-service-message')).to_have_text('IDA MCP started successfully.')
            expect(page.locator('#workbench-report')).to_be_hidden()
            page.locator('#language').click()
            expect(page.locator('#ida-status')).to_contain_text('支持打开库')
            expect(page.locator('#open-ida')).to_be_enabled()
            self.assertEqual(endpoint['url'],'http://127.0.0.1:13338/mcp')
            browser.close()

    def test_ida_startup_failure_refreshes_status_preserves_analysis_and_allows_retry(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url,*_),sync_playwright() as p:
            browser=p.chromium.launch(channel=os.environ.get('UMT_BROWSER','msedge'),headless=True)
            page=browser.new_page(viewport={'width':390,'height':844})
            fixture={'ready':False,'attempt':0,'refreshes':0,'connected':False}
            commands=[]
            errors=[]
            page.on('pageerror',lambda error:errors.append(str(error)))
            def status_route(route):
                fixture['refreshes']+=1
                route.fulfill(content_type='application/json',body=json.dumps({'version':'fixture',
                    'sessions':[{'id':'fixture-session'}],'jobs':[],
                    'ida':{'connected':fixture['connected'],'canOpenDatabase':fixture['connected']}}))
            def command_route(route):
                body=route.request.post_data_json
                commands.append(body)
                if body['command']=='start_ida':
                    fixture['attempt']+=1
                    fixture['ready']=False
                    route.fulfill(content_type='application/json',body=json.dumps({'jobId':'ida-retry','status':'queued'}))
                elif body['command']=='ida_status':
                    route.fulfill(content_type='application/json',body=json.dumps({'connected':False}))
                else:route.continue_()
            def job_route(route):
                result={'id':'ida-retry','kind':'start_ida','status':'running'}
                if fixture['ready']:
                    if fixture['attempt']==1:
                        result.update(status='error',error='Install ida-pro-mcp with the IDA Python environment, then retry.')
                    else:
                        fixture['connected']=True
                        result.update(status='done',result={'ida':{'connected':True,'canOpenDatabase':True},
                            'idaUrl':'http://127.0.0.1:13338/mcp',
                            'service':{'started':False,'reused':True,'message':'Connected to the running IDA MCP service.'}})
                route.fulfill(content_type='application/json',body=json.dumps(result))
            page.route('**/api/workbench/status',status_route)
            page.route('**/api/workbench/command',command_route)
            page.route('**/api/workbench/job?id=ida-retry',job_route)
            page.goto(url)
            expect(page.locator('#workbench-content')).to_be_visible()
            expect(page.locator('#ida-endpoint')).to_have_value('http://127.0.0.1:13338/mcp')
            page.locator('#sync-dump').click()
            expect(page.locator('#analysis-decompile')).to_contain_text('self->Health')
            page.locator('#ida-endpoint-save').click()
            expect(page.locator('#ida-endpoint-save')).to_be_disabled()
            fixture['ready']=True
            expect(page.locator('#workbench-error')).to_contain_text('Install ida-pro-mcp')
            expect(page.locator('#ida-endpoint-save')).to_be_enabled()
            expect(page.locator('#connect-ida')).to_be_enabled()
            expect(page.locator('#analysis-decompile')).to_contain_text('self->Health')
            self.assertGreaterEqual(fixture['refreshes'],3)
            page.locator('#connect-ida').click()
            expect(page.locator('#connect-ida')).to_be_enabled()
            self.assertEqual(commands[-1]['command'],'ida_status')
            self.assertEqual(fixture['attempt'],1)
            page.locator('#ida-endpoint-save').click()
            expect(page.locator('#ida-endpoint-save')).to_have_text('正在启动…')
            fixture['ready']=True
            expect(page.locator('#ida-status')).to_contain_text('支持打开库')
            expect(page.locator('#ida-service-message')).to_have_text('Connected to the running IDA MCP service.')
            expect(page.locator('#workbench-error')).to_be_hidden()
            expect(page.locator('#analysis-decompile')).to_contain_text('self->Health')
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
            self.assertEqual(errors,[])
            browser.close()

    def test_workbench_sync_search_analyze_evidence_and_agent_prompt(self):
        from playwright.sync_api import sync_playwright, expect
        import urllib.request
        with fake_server() as (url, *_), sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel=os.environ.get("UMT_BROWSER", "msedge"), headless=True)
            context = browser.new_context(viewport={"width": 1440, "height": 1080}, permissions=["clipboard-read", "clipboard-write"])
            page = context.new_page()
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(url)
            expect(page.locator("#workbench-content")).to_be_visible()
            self.assertFalse(page.locator("#symbols-only").is_checked())
            page.locator("#sync-dump").click()
            expect(page.locator("#workbench-job")).to_have_text("任务完成")
            page.locator("#function-query").fill("GetHealth")
            page.locator("#find-functions").click()
            expect(page.locator(".function-choice")).to_have_count(1)
            page.locator(".function-choice").click()
            expect(page.locator("#selected-function-rva")).to_have_text("RVA 0xF123456789ABCDEF")
            page.locator("#analyze-function").click()
            expect(page.locator("#analysis-decompile")).to_contain_text("self->Health")
            self.assertNotIn('"code":', page.locator("#analysis-decompile").inner_text())
            expect(page.locator("#analysis-xrefs")).to_contain_text("UpdateHud")
            page.locator("#agent-goal").fill("解释 Health 字段与 HUD 更新链路，给出等价实现。")
            page.locator("#build-agent-task").click()
            expect(page.locator("#agent-prompt")).to_be_visible()
            self.assertIn("0xF123456789ABCDEF", page.locator("#agent-prompt").input_value())
            self.assertIn("self->Health", page.locator("#agent-prompt").input_value())
            page.locator("#copy-agent-task").click()
            expect(page.locator("#agent-copy-status")).to_have_text("已复制")
            with urllib.request.urlopen(url + "/api/workbench/status") as response:
                commands = json.load(response)["commands"]
            self.assertEqual(commands[0]["symbolsOnly"], False)
            self.assertEqual(commands[-1]["rva"], "0xF123456789ABCDEF")
            screenshots = ROOT / "desktop/screenshots"
            screenshots.mkdir(parents=True, exist_ok=True)
            page.locator("#workbench").screenshot(path=str(screenshots / "workbench-desktop-fixture.png"), animations="disabled")
            page.set_viewport_size({"width": 390, "height": 844})
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
            page.locator("#workbench").screenshot(path=str(screenshots / "workbench-mobile-fixture.png"), animations="disabled")
            self.assertEqual(errors, [])
            browser.close()

    def test_sdk_explorer_search_paging_inspect_history_tags_and_generation(self):
        from playwright.sync_api import sync_playwright, expect
        with fake_server() as (url, state, calls, _), sync_playwright() as playwright:
            state.update(probed=True, pid=22480, package="com.tencent.tmgp.pubgmhd")
            browser = playwright.chromium.launch(channel=os.environ.get("UMT_BROWSER", "msedge"), headless=True)
            page = browser.new_page(viewport={"width": 1440, "height": 1080})
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(url)
            expect(page.locator('[data-action="dump"]')).to_be_enabled()
            page.locator("#tab-explorer").click()
            expect(page.locator(".object-select")).to_have_count(100)
            page.locator("#object-limit").select_option("25")
            expect(page.locator(".object-select")).to_have_count(25)
            expect(page.locator(".object-select").first).to_contain_text("PlayerPawn_0")
            page.locator("#object-next").click()
            expect(page.locator(".object-select").first).to_contain_text("PlayerPawn_25")
            page.locator("#object-prev").click()
            expect(page.locator(".object-select").first).to_contain_text("PlayerPawn_0")
            page.locator(".object-select").first.click()
            expect(page.locator("#object-title")).to_have_text("PlayerPawn_0")
            expect(page.locator("#inspect-address")).to_have_value("0xF123456789AB0000")
            page.locator("#object-tag").click()
            expect(page.locator(".bookmark-open")).to_have_count(1)
            page.locator(".pointer-button").filter(has_text="PlayerController_2").click()
            expect(page.locator("#object-title")).to_have_text("PlayerPawn_2")
            page.locator("#inspect-back").click()
            expect(page.locator("#object-title")).to_have_text("PlayerPawn_0")
            page.locator('details[data-section="functions"] summary').click()
            expect(page.locator(".function-signature")).to_have_text("float GetHealth() const")
            page.locator('details[data-section="containers"] summary').click()
            expect(page.locator(".container-card")).to_contain_text("Inventory")
            page.locator('details[data-section="runtime"] summary').click()
            expect(page.locator(".runtime-content")).to_contain_text("RootComponent")
            page.locator("#inspect-address").fill("0xF123456789AB0007")
            page.locator("#inspect-kind").select_option("field")
            page.locator("#inspect-go").click()
            expect(page.locator("#object-title")).to_have_text("HealthProperty")
            page.locator(".bookmark-open").click()
            expect(page.locator("#object-title")).to_have_text("PlayerPawn_0")
            page.locator("#object-query").fill("PlayerPawn_259")
            page.locator("#object-search").click()
            expect(page.locator(".object-select")).to_have_count(1)
            expect(page.locator(".object-select").first).to_contain_text("PlayerPawn_259")
            searches = [call for call in calls if call.get("endpoint") == "explorer" and call.get("query") == "PlayerPawn_259"]
            self.assertGreater(len(searches), 1)
            self.assertTrue(all(call["scanLimit"] == 4096 for call in searches))
            pointers = [call["address"] for call in calls if call.get("action") == "inspect"]
            self.assertIn("0xF123456789AB0007", pointers)
            self.assertTrue(all(isinstance(address, str) for address in pointers))
            page.locator("#inspect-address").fill("0xF123456789ABDEAD")
            page.locator("#inspect-go").click()
            expect(page.locator("#explorer-error")).to_have_text("Object memory is unavailable.")
            expect(page.locator("#object-title")).to_have_text("PlayerPawn_0")
            page.locator("#object-query").fill("fixtureSlow")
            page.locator("#object-search").click()
            expect(page.locator("#object-stop")).to_be_visible()
            page.locator("#object-stop").click()
            expect(page.locator("#object-stop")).to_be_hidden()
            expect(page.locator("#object-search")).to_be_enabled()
            page.locator("#object-query").fill("PlayerPawn_259")
            page.locator("#object-search").click()
            expect(page.locator(".object-select")).to_have_count(1)
            expect(page.locator("#object-search")).to_be_enabled()
            screenshots = ROOT / "desktop/screenshots"
            screenshots.mkdir(exist_ok=True)
            page.locator("#view-explorer").screenshot(path=str(screenshots / "explorer-desktop-fixture.png"), animations="disabled")
            page.set_viewport_size({"width": 390, "height": 844})
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
            page.locator("#view-explorer").screenshot(path=str(screenshots / "explorer-mobile-fixture.png"), animations="disabled")
            state["generation"] += 1
            expect(page.locator(".bookmark-open")).to_have_count(0, timeout=4000)
            expect(page.locator("#object-detail")).to_be_hidden()
            self.assertEqual(errors, [])
            browser.close()

    def test_real_browser_actions_search_languages_and_mobile(self):
        from playwright.sync_api import sync_playwright, expect
        errors = []
        with fake_server() as (url, state, calls, files), sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel=os.environ.get("UMT_BROWSER", "msedge"), headless=True)
            page = browser.new_page(viewport={"width": 1440, "height": 1100}, device_scale_factor=1)
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(url)
            expect(page.locator("#connection")).to_have_class("connection online")
            expect(page.locator('[data-action="probe"]')).to_be_enabled()
            expect(page.locator('[data-action="dump"]')).to_be_disabled()
            page.locator('[data-action="probe"]').click()
            expect(page.locator('[data-action="dump"]')).to_be_enabled()
            expect(page.locator("#offset-rows tr")).to_have_count(4)
            page.locator("#search").fill("GWorld")
            expect(page.locator("#offset-rows tr")).to_have_count(1)
            page.locator("#search").fill("")
            page.locator('[data-action="dump"]').click()
            expect(page.locator("#files .download")).to_have_count(3, timeout=6000)
            self.assertEqual(calls[:2], [{"action": "probe", "pid": 22480}, {"action": "dump", "pid": 22480}])
            expect(page.locator("#objects-bar")).to_have_attribute("aria-valuenow", "100")
            state["busy"] = True
            expect(page.locator('[data-action="probe"]')).to_be_disabled(timeout=4000)
            expect(page.locator("#process")).to_be_disabled()
            files.append({"name": "Repeated-export.txt", "size": 4, "url": "/download/Repeated-export.txt"})
            state["busy"] = False
            expect(page.locator("#files .download")).to_have_count(4, timeout=4000)
            files.pop()
            page.locator("#tab-structures").click()
            page.locator("#groups summary").click()
            expect(page.locator("#groups td").filter(has_text="ClassPrivate")).to_be_visible()
            page.locator("#tab-logs").click()
            expect(page.locator("#log-lines")).to_contain_text("<img src=x onerror=alert(1)>")
            self.assertEqual(page.locator("#log-lines img").count(), 0)
            page.locator("#clear-logs").click()
            expect(page.locator("#log-empty")).to_be_visible()
            page.locator("#tab-offsets").click()
            # Screenshots are clearly labelled synthetic fixtures in the UI version.
            screenshots = ROOT / "desktop/screenshots"
            screenshots.mkdir(exist_ok=True)
            page.evaluate("localStorage.setItem('umt-theme','dark')")
            if page.locator("html").get_attribute("data-theme") != "dark":
                page.locator("#theme").click()
            page.screenshot(path=str(screenshots / "desktop-dark-fixture.png"), full_page=True, animations="disabled")
            page.locator("#language").click()
            expect(page.locator("h1")).to_have_text("Memory, made clear.")
            page.locator("#theme").click()
            expect(page.locator("html")).to_have_attribute("data-theme", "light")
            page.screenshot(path=str(screenshots / "desktop-light-fixture.png"), full_page=True, animations="disabled")
            page.set_viewport_size({"width": 390, "height": 844})
            page.locator("#language").click()
            page.locator("#theme").click()
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= window.innerWidth"))
            page.screenshot(path=str(screenshots / "mobile-fixture.png"), full_page=True, animations="disabled")
            self.assertEqual(errors, [])
            browser.close()


if __name__ == "__main__":
    unittest.main()
