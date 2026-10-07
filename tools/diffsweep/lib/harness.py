"""Starts and resets the two apps (expected and actual) on VM-local copies of a seed."""
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import time
import urllib.request

# The folder that holds this repo and once-campfire-rust.
WORKSPACE = os.environ.get("CAMPFIRE_WORKSPACE") or os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
SEED_ROOT = f"{WORKSPACE}/once-campfire-rust/parity/.seed"
ENV_FILE = f"{WORKSPACE}/once-campfire-rust/parity/.env.reference"
BENCH_DIR = "/var/lib/campfire-bench"
CONTAINER_PREFIX = os.environ.get("DIFFSWEEP_PREFIX", "diffsweep")  # one prefix for each user: names must not collide
APP_USER = "1000:1000"
FAKETIME_LIB = "/usr/local/lib/faketime/libfaketime.so.1"


def log(msg):
    print(f"[{time.strftime('%T')}] {msg}", file=sys.stderr, flush=True)


def sh(*args, check=True):
    p = subprocess.run(args, capture_output=True, text=True)
    if check and p.returncode != 0:
        raise RuntimeError(f"{' '.join(args)[:200]} failed: {p.stderr.strip()[:500]}")
    return p


def seed_labels(seed):
    path = f"{SEED_ROOT}/{seed}/labels.json"
    if not os.path.exists(path):
        raise SystemExit(f"seed '{seed}' is not built ({path}); run once-campfire-rust/parity/bin/seed build {seed}")
    return json.load(open(path))


def env_file_vars():
    out = []
    for line in open(ENV_FILE):
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line.split("=", 1))
    return out


def neuter_deliveries(db_path):
    """Push endpoints and webhook URLs point at a closed local port (as in bench/run)."""
    db = sqlite3.connect(db_path)
    db.execute("UPDATE push_subscriptions SET endpoint = 'https://127.0.0.1:9/push/' || id")
    db.execute("UPDATE webhooks SET url = 'http://127.0.0.1:9/hook/' || id")
    db.commit()
    db.close()


def image_has_faketime(image):
    return sh("docker", "run", "--rm", "--entrypoint", "test", image, "-f", FAKETIME_LIB, check=False).returncode == 0


class App:
    def __init__(self, role, image, port, tag):
        self.role, self.image, self.port = role, image, port
        self.container = f"{CONTAINER_PREFIX}-{tag}-{role}"
        self.dir = f"{BENCH_DIR}/diffsweep/{CONTAINER_PREFIX}-{tag}-{role}-{os.getpid()}"  # the PID is 1 in each sweep container
        self.faketime = image_has_faketime(image)

    def start(self, seed, extra_env=()):
        self.stop(keep_dir=False)
        labels = seed_labels(seed)
        now = labels["clock.now"]  # 2026-03-02T16:00:00Z
        os.makedirs(f"{self.dir}/db", exist_ok=True)
        os.makedirs(f"{self.dir}/storage", exist_ok=True)
        sh("cp", "-a", f"{SEED_ROOT}/{seed}/db/.", f"{self.dir}/db/")
        sh("cp", "-a", f"{SEED_ROOT}/{seed}/storage/.", f"{self.dir}/storage/")
        neuter_deliveries(f"{self.dir}/db/production.sqlite3")
        sh("chown", "-R", APP_USER, self.dir)
        env = dict(env_file_vars())
        env.update({"HTTP_PORT": str(self.port), "TARGET_PORT": str(self.port + 1000),
                    "CAMPFIRE_FROZEN_TIME": now})
        if self.faketime:  # Rails: libfaketime frozen at the seed instant, one process (as parity/bin/reference)
            env["FAKETIME"] = now.replace("T", " ").replace("Z", "")
            env["WEB_CONCURRENCY"] = "0"
        for kv in extra_env:
            k, v = kv.split("=", 1)
            env[k] = v
        if self.role == "actual":  # DIFFSWEEP_ACTUAL_ENV="A=b;C=d": options of a sanitizer image
            for kv in filter(None, os.environ.get("DIFFSWEEP_ACTUAL_ENV", "").split(";")):
                k, v = kv.split("=", 1)
                env[k] = v
        args = ["docker", "run", "-d", "--name", self.container, "--user", APP_USER, "--network", "host",
                "--label", "t13.diffsweep=1"]
        if self.role == "actual" and os.environ.get("DIFFSWEEP_SANITIZER_RUN"):
            args += ["--security-opt", "seccomp=unconfined"]  # TSan needs the personality system call
        for k, v in env.items():
            args += ["-e", f"{k}={v}"]
        args += ["-v", f"{self.dir}/db:/rails/storage/db", "-v", f"{self.dir}/storage:/rails/storage/files",
                 self.image]
        sh("sync", check=False)
        sh(*args)
        self.wait_up()

    def wait_up(self, tries=2400):
        for _ in range(tries):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{self.port}/up", timeout=2).read()
                return
            except Exception:
                time.sleep(0.05)
        logs = sh("docker", "logs", "--tail", "30", self.container, check=False)
        raise RuntimeError(f"{self.container} ({self.image}) did not come up:\n{logs.stdout}{logs.stderr}")

    def stop(self, keep_dir=False):
        save = os.environ.get("DIFFSWEEP_SAVE_LOGS")  # a directory: keep the log of each actual app
        if save and self.role == "actual":
            log = sh("docker", "logs", self.container, check=False)
            if log.stdout or log.stderr:
                os.makedirs(save, exist_ok=True)
                with open(f"{save}/{self.container}-{os.getpid()}.log", "a") as out:
                    out.write(log.stdout + log.stderr)
        sh("docker", "rm", "-f", self.container, check=False)
        if not keep_dir:
            shutil.rmtree(self.dir, ignore_errors=True)

    def logs(self, tail=40):
        p = sh("docker", "logs", "--tail", str(tail), self.container, check=False)
        return p.stdout + p.stderr


class Pair:
    """The expected and the actual app, started and reset together."""

    def __init__(self, expected_image, actual_image, base_port, tag):
        self.expected = App("expected", expected_image, base_port, tag)
        self.actual = App("actual", actual_image, base_port + 1, tag)
        self.seed = None
        self.logins = 0

    @property
    def sides(self):
        return (self.expected, self.actual)

    def start(self, seed, extra_env=()):
        self.seed, self.logins = seed, 0
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(2) as ex:
            list(ex.map(lambda a: a.start(seed, extra_env), self.sides))

    def stop(self):
        for a in self.sides:
            a.stop()
