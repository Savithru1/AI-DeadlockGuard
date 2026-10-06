"""
dashboard/app.py — Phase 12

Flask backend for the AI-DeadlockGuard live dashboard.
Serves the frontend and provides:
  GET  /        — index.html
  GET  /stream  — Server-Sent Events (pushed every 500ms)
  GET  /api/state  — single JSON snapshot
  POST /api/resolve — write trigger file → Phase 7 manual kill
"""

import os
import json
import time
import threading
from datetime import datetime
from flask import Flask, render_template, Response, jsonify, request
from flask_cors import CORS

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

FEATURES_PATH  = os.path.join(ROOT_DIR, 'status', 'features.json')
HEARTBEAT_PATH = os.path.join(ROOT_DIR, 'status', 'heartbeat.json')
RISK_PATH      = os.path.join(ROOT_DIR, 'status', 'risk.json')
LOG_PATH       = os.path.join(ROOT_DIR, 'logs', 'monitor_events.log')
RESOLVE_PATH   = '/tmp/deadlock_guard.resolve'

# On Windows, store the resolve trigger inside the project instead
if os.name == 'nt':
    RESOLVE_PATH = os.path.join(ROOT_DIR, 'status', 'resolve.trigger')

app = Flask(__name__)
CORS(app)

# ── Helpers ────────────────────────────────────────────────────────────────

def _read_json(path, default=None):
    try:
        with open(path, 'r') as f:
            return json.load(f)
    except Exception:
        return default or {}


def _read_log_tail(path, n=50):
    """Read last n lines from the log file."""
    lines = []
    try:
        with open(path, 'r', errors='replace') as f:
            lines = f.readlines()
    except Exception:
        pass
    return [l.rstrip() for l in lines[-n:]]


def _build_state():
    """Merge all status files into a single state dict."""
    features  = _read_json(FEATURES_PATH,  {'blocked_count': 0, 'wait_time_growth': 0.0,
                                             'edge_count': 0, 'graph_density': 0.0})
    heartbeat = _read_json(HEARTBEAT_PATH, {'tick': 0, 'status': 'stopped'})
    risk      = _read_json(RISK_PATH,      {'risk_score': 0.0, 'action': 'none'})
    log_lines = _read_log_tail(LOG_PATH)

    monitor_status = heartbeat.get('status', 'stopped')

    return {
        'timestamp': datetime.now().isoformat(),
        'monitor_status': monitor_status,
        'tick': heartbeat.get('tick', 0),
        'features': {
            'blocked_count':    features.get('blocked_count', 0),
            'wait_time_growth': round(features.get('wait_time_growth', 0.0), 4),
            'edge_count':       features.get('edge_count', 0),
            'graph_density':    round(features.get('graph_density', 0.0), 6),
        },
        'risk_score':  round(risk.get('risk_score', 0.0), 4),
        'action':      risk.get('action', 'none'),
        'log_lines':   log_lines,
    }


# ── Routes ─────────────────────────────────────────────────────────────────

@app.route('/')
def index():
    return render_template('index.html')


@app.route('/api/state')
def api_state():
    return jsonify(_build_state())


INJECT_PATH = os.path.join(ROOT_DIR, 'status', 'inject_deadlock.trigger')

@app.route('/api/resolve', methods=['POST'])
def api_resolve():
    """Write the trigger file so the C monitor fires Phase 7 manual kill."""
    try:
        os.makedirs(os.path.dirname(RESOLVE_PATH), exist_ok=True)
        with open(RESOLVE_PATH, 'w') as f:
            f.write('1')
        return jsonify({'ok': True, 'message': 'Manual resolve triggered.'})
    except Exception as e:
        return jsonify({'ok': False, 'error': str(e)}), 500


@app.route('/api/create-deadlock', methods=['POST'])
def api_create_deadlock():
    """Trigger a realistic deadlock injection sequence with optional auto-resolve."""
    try:
        data = request.get_json(silent=True) or {}
        auto_resolve = data.get('auto_resolve', True)

        payload = {
            'timestamp': time.time(),
            'auto_resolve': bool(auto_resolve)
        }
        os.makedirs(os.path.dirname(INJECT_PATH), exist_ok=True)
        with open(INJECT_PATH, 'w', encoding='utf-8') as f:
            json.dump(payload, f)

        msg = 'Deadlock creation started (Auto-resolve ENABLED).' if auto_resolve else 'Deadlock creation started (Auto-resolve DISABLED — system will stay frozen).'
        return jsonify({'ok': True, 'message': msg, 'auto_resolve': auto_resolve})
    except Exception as e:
        return jsonify({'ok': False, 'error': str(e)}), 500


@app.route('/stream')
def stream():
    """
    Server-Sent Events endpoint. Pushes a state snapshot every 500ms.
    The browser opens EventSource('/stream') once; updates arrive automatically.
    """
    def event_generator():
        while True:
            state = _build_state()
            data  = json.dumps(state)
            yield f'data: {data}\n\n'
            time.sleep(0.5)

    return Response(
        event_generator(),
        mimetype='text/event-stream',
        headers={
            'Cache-Control': 'no-cache',
            'X-Accel-Buffering': 'no',
        }
    )


# ── Entry point ─────────────────────────────────────────────────────────────

if __name__ == '__main__':
    # Ensure required directories exist
    os.makedirs(os.path.join(ROOT_DIR, 'status'), exist_ok=True)
    os.makedirs(os.path.join(ROOT_DIR, 'logs'), exist_ok=True)

    print("=" * 55)
    print("  AI-DeadlockGuard Dashboard")
    print("  Open  http://localhost:5050  in your browser")
    print("=" * 55)
    app.run(debug=False, threaded=True, host='0.0.0.0', port=5050)
