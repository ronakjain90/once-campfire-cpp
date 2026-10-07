import sys, os
sys.path.insert(0, "/Volumes/ExternalHD/Code/AI/once-campfire/wt/A2b/tools/diffsweep/lib")
import harness
from runner import Run
a_img, b_img, path, outdir = sys.argv[1:5]
pair = harness.Pair(a_img, b_img, 4590, "dump")
pair.start("default", [])
try:
    run = Run(pair, harness.seed_labels("default"), "127.0.0.1:4590", {"ignored": [], "normalizers": []})
    ctx = {"area": "P", "list": "x", "scenario": "s", "scenario_def": {}}
    run.do_login(ctx, {"actor": "david"})
    p = path
    for key, val in harness.seed_labels("default").items():
        p = p.replace("{{" + key + "}}", str(val))
    resps = run.do_request(ctx, {"actor": "david", "path": p, "revisit": False})
    resps = run.do_request(ctx, {"actor": "david", "path": p, "revisit": False})
    for name, r in zip(("expected", "actual"), resps):
        open(f"{outdir}/{name}.html", "wb").write(r.body)
        print(name, r.status, len(r.body), r.get("etag"), r.get("x-cache"))
finally:
    pair.stop()
