import sys, json
sys.path.insert(0, "/Volumes/ExternalHD/Code/AI/once-campfire/wt/A2b/tools/diffsweep/lib")
import harness
from runner import Run
pair = harness.Pair("campfire-rust:app", sys.argv[1], 4590, "dump")
pair.start("default", [])
try:
    run = Run(pair, harness.seed_labels("default"), "127.0.0.1:4590", {"ignored": [], "normalizers": []})
    ctx = {"area": "P", "list": "x", "scenario": "s", "scenario_def": {}}
    run.do_login(ctx, {"actor": "david"})
    d = json.load(open("/Volumes/ExternalHD/Code/AI/once-campfire/wt/A2b/tools/diffsweep/lists/writes-A3.json"))
    sc = [s for s in d["scenarios"] if s["name"] == "attachment"][0]
    step = dict(sc["steps"][1]); step.pop("capture", None)
    import copy
    if len(sys.argv) > 2 and sys.argv[2] == "nofile":
        step = copy.deepcopy(step); step["multipart"]["files"] = []
    if len(sys.argv) > 2 and sys.argv[2] == "bmp":
        step = copy.deepcopy(sc["steps"][2])
    resps = run.do_request(ctx, step)
    for r in resps:
        print(r.status, r.text()[:300])
    print(pair.actual.logs(30))
finally:
    pair.stop()
