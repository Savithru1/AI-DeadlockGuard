#!/usr/bin/env python3
"""
AI Deadlock Guard -- Flask web server.

The C program (deadlock_guard) does all the work and owns every process.
Every 100 ms it writes a JSON snapshot of its state to
/tmp/deadlock_guard_state.json; it also appends to logs/events.log and
logs/results.csv. This server reads those files and serves them to the web
dashboard, and passes run requests from the dashboard's buttons back to
the C program through a small command file it polls.

Endpoints:
  GET  /             -> the web dashboard (static/index.html)
  GET  /api/health   -> is the server up, and is the C backend running?
  GET  /api/state    -> latest snapshot (run, AI risk, processes, timeline, results)
  GET  /stream       -> the same snapshot pushed every 300 ms (Server-Sent Events)
  GET  /api/results  -> every completed run so far (logs/results.csv)
  POST /api/run      -> {"mode": "baseline" | "ai" | "compare"} start a run
  POST /api/resolve  -> manual override: kill one deadlock victim now

Run it inside WSL, next to the C program:
  python3 app.py            (or: make web)
then open http://localhost:8088 in a browser on Windows.
"""

import csv
import json
import os
import time

from flask import Flask, Response, jsonify, request

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATE_FILE = os.environ.get("DLG_STATE_FILE", "/tmp/deadlock_guard_state.json")
COMMAND_FILE = os.environ.get("DLG_COMMAND_FILE", "/tmp/deadlock_guard.cmd")
RESULTS_LOG = os.path.join(BASE_DIR, "logs", "results.csv")

PORT = int(os.environ.get("DLG_WEB_PORT", "8088"))

# The C program rewrites the snapshot every 100 ms. If it is older than
# this, assume the backend has stopped.
STALE_AFTER_SEC = 3
STREAM_INTERVAL_SEC = 0.3

app = Flask(__name__, static_folder="static", static_url_path="/static")


def read_snapshot():
    """Returns (data, error_message). Exactly one of them is None."""
    if not os.path.exists(STATE_FILE):
        return None, "No data yet. Start the C program in WSL: ./deadlock_guard"
    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            return json.load(f), None
    except (OSError, json.JSONDecodeError) as e:
        return None, "Could not read snapshot: {}".format(e)


def backend_is_running(snapshot):
    if snapshot is None:
        return False
    return (time.time() - snapshot.get("timestamp", 0)) <= STALE_AFTER_SEC


def current_state():
    snapshot, error = read_snapshot()
    if snapshot is None:
        return {"backend_running": False, "error": error}
    snapshot["backend_running"] = backend_is_running(snapshot)
    if not snapshot["backend_running"]:
        snapshot["error"] = "The C backend stopped. Start it again: ./deadlock_guard"
    return snapshot


def send_command(command):
    """Hand a command to the C program (it polls this file every 10 ms)."""
    tmp = COMMAND_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(command + "\n")
    os.replace(tmp, COMMAND_FILE)   # atomic: the C side never sees half a line


@app.route("/")
def dashboard():
    return app.send_static_file("index.html")


@app.route("/api/health")
def health():
    snapshot, _ = read_snapshot()
    return jsonify({"server": "ok", "backend_running": backend_is_running(snapshot)})


@app.route("/api/state")
def state():
    data = current_state()
    return jsonify(data), (200 if data["backend_running"] else 503)


@app.route("/stream")
def stream():
    def events():
        while True:
            yield "data: {}\n\n".format(json.dumps(current_state()))
            time.sleep(STREAM_INTERVAL_SEC)

    return Response(events(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/results")
def results():
    rows = []
    if os.path.exists(RESULTS_LOG):
        with open(RESULTS_LOG, newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
    return jsonify(rows[-50:])


@app.route("/api/run", methods=["POST"])
def run():
    mode = (request.get_json(silent=True) or {}).get("mode")
    if mode not in ("baseline", "ai", "compare"):
        return jsonify({"ok": False, "error": "mode must be baseline, ai or compare"}), 400

    data = current_state()
    if not data["backend_running"]:
        return jsonify({"ok": False, "error": data["error"]}), 503
    run_info = data.get("run", {})
    if run_info.get("phase") == "active" or run_info.get("pending") or run_info.get("compare_stage"):
        return jsonify({"ok": False, "error": "A run is already in progress."}), 409
    if mode != "baseline" and data.get("ai", {}).get("status") == "unavailable":
        return jsonify({"ok": False, "error": "AI model unavailable: " + data["ai"].get("message", "")}), 409

    send_command("run " + mode)
    return jsonify({"ok": True, "message": "Run requested — it is also shown in the WSL terminal."})


@app.route("/api/resolve", methods=["POST"])
def resolve():
    data = current_state()
    if not data["backend_running"]:
        return jsonify({"ok": False, "error": data["error"]}), 503
    if data.get("run", {}).get("phase") != "active":
        return jsonify({"ok": False, "error": "No run in progress."}), 409
    send_command("resolve")
    return jsonify({"ok": True, "message": "Manual resolve sent: the monitor will SIGKILL one victim."})


if __name__ == "__main__":
    # host="0.0.0.0": reachable from the Windows browser via WSL's
    # localhost forwarding (http://localhost:8088).
    print("AI Deadlock Guard website: http://localhost:{}".format(PORT))
    app.run(host="0.0.0.0", port=PORT, debug=False, threaded=True)
