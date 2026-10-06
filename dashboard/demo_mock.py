#!/usr/bin/env python3
"""
demo_mock.py — Demo mode data generator with On-Demand Deadlock Injection

Writes mock features.json, heartbeat.json, risk.json, and monitor_events.log
to the status/ and logs/ directories.

Features:
- Standard background cycle: safe -> mild contention -> recovery
- On-demand injection: when status/inject_deadlock.trigger is touched (via GUI button),
  it executes a 10-15s sequence:
    0-4s: Normal execution
    4-8s: Contention build-up (risk rises to 65%)
    8-11s: Circular wait forms! Deadlock detected (risk hits 94%, DEADLOCK banner flashes)
    11-14s: Auto-detection and auto-resolution fires! (victim killed, cycle broken, returns to safe green)
- Responds immediately if manual Resolve Deadlock trigger is detected.
"""

import os
import sys
import json
import time
import random
from datetime import datetime

# Prevent Windows console charmap encoding errors
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT_DIR            = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
FEATURES_PATH       = os.path.join(ROOT_DIR, 'status', 'features.json')
HEARTBEAT_PATH      = os.path.join(ROOT_DIR, 'status', 'heartbeat.json')
RISK_PATH           = os.path.join(ROOT_DIR, 'status', 'risk.json')
LOG_PATH            = os.path.join(ROOT_DIR, 'logs', 'monitor_events.log')
INJECT_TRIGGER_PATH = os.path.join(ROOT_DIR, 'status', 'inject_deadlock.trigger')
RESOLVE_TRIGGER_PATH= os.path.join(ROOT_DIR, 'status', 'resolve.trigger')
TMP_RESOLVE_PATH    = '/tmp/deadlock_guard.resolve'

MAX_NODES = 64
MAX_EDGES = MAX_NODES * (MAX_NODES - 1)
RESOURCES = ['mutex_A', 'mutex_B', 'fork_0', 'fork_1', 'semaphore_1', 'rwlock_X']

os.makedirs(os.path.join(ROOT_DIR, 'status'), exist_ok=True)
os.makedirs(os.path.join(ROOT_DIR, 'logs'),   exist_ok=True)

def write_status(blocked, wg, edges, risk, status, action):
    density = round(edges / MAX_EDGES, 6)
    with open(FEATURES_PATH, 'w') as f:
        json.dump({
            'blocked_count':    blocked,
            'wait_time_growth': round(wg, 4),
            'edge_count':       edges,
            'graph_density':    density,
        }, f)

    with open(HEARTBEAT_PATH, 'w') as f:
        json.dump({'tick': int(time.time()), 'status': status}, f)

    with open(RISK_PATH, 'w') as f:
        json.dump({
            'timestamp': datetime.now().isoformat(),
            'risk_score': round(min(max(risk, 0.0), 1.0), 4),
            'action': action,
        }, f)

def append_log(event_type, msg):
    ts = datetime.now().strftime('[%H:%M:%S]')
    with open(LOG_PATH, 'a', encoding='utf-8') as f:
        f.write(f'{ts} {event_type} {msg}\n')

def check_manual_resolve():
    for p in [RESOLVE_TRIGGER_PATH, TMP_RESOLVE_PATH]:
        if os.path.exists(p):
            try:
                os.remove(p)
            except:
                pass
            return True
    return False

def run_injected_deadlock_sequence(auto_resolve=True):
    """
    Executes a realistic deadlock scenario:
    - 0-4s: Safe normal baseline
    - 4-8s: Rapid contention spike (Thread A requests Mutex B)
    - 8s+: Circular wait forms! Deadlock detected (Thread B requests Mutex A)
    - If auto_resolve=True: Auto-resolution breaks the deadlock cycle at 12s
    - If auto_resolve=False: System stays frozen in deadlock until manual resolve clicked!
    """
    mode_str = "auto-resolve ENABLED" if auto_resolve else "auto-resolve DISABLED (Freeze)"
    print(f"\n[demo_mock] [INJECT] INJECTING DEADLOCK SEQUENCE ({mode_str})...")
    pid_a = 4101
    pid_b = 4102

    append_log('MONITOR', f'Scenario initiated: Dining Philosophers 2-Thread Contention [{mode_str}]')
    append_log('HOLD', f'pid={pid_a} resource=fork_0')
    append_log('HOLD', f'pid={pid_b} resource=fork_1')

    start = time.time()
    deadlock_detected = False
    resolved = False

    while True:
        elapsed = time.time() - start

        # Check if user clicked manual resolve at any time
        if check_manual_resolve():
            append_log('RESOLVE', f'Manual resolution signal received — terminating cycle victim pid={pid_b}')
            append_log('RELEASE', f'resource=fork_1 released by kernel')
            append_log('HOLD', f'pid={pid_a} acquired resource=fork_1, thread resumed')
            resolved = True
            break

        # Stage 1: 0 to 4 seconds — Safe baseline
        if elapsed < 4.0:
            write_status(
                blocked=0,
                wg=0.04,
                edges=2,
                risk=0.12,
                status='running',
                action='none'
            )
            if int(elapsed * 2) % 2 == 0:
                append_log('MONITOR', f'Healthcheck: tick ok, active threads: [{pid_a}, {pid_b}]')

        # Stage 2: 4 to 8 seconds — Contention escalates
        elif elapsed < 8.0:
            prog = (elapsed - 4.0) / 4.0
            if prog > 0.1 and prog < 0.3:
                append_log('WAIT', f'pid={pid_a} resource=fork_1 (held by pid={pid_b})')
            write_status(
                blocked=1,
                wg=0.25 + prog * 0.45,
                edges=3,
                risk=0.35 + prog * 0.35, # climbing to 0.70
                status='running',
                action='none'
            )

        # Stage 3: 8 seconds and beyond — Circular wait completes! Deadlock!
        else:
            if not deadlock_detected:
                deadlock_detected = True
                append_log('WAIT', f'pid={pid_b} resource=fork_0 (held by pid={pid_a})')
                append_log('DEADLOCK', f'Cycle detected: [pid {pid_a} -> fork_1 -> pid {pid_b} -> fork_0 -> pid {pid_a}]')
                if auto_resolve:
                    append_log('DEADLOCK', f'AI Ensemble score: 94.2% — Imminent deadlock threshold crossed! Auto-resolve armed.')
                else:
                    append_log('DEADLOCK', f'AI Ensemble score: 94.2% — Auto-resolution DISABLED. Threads {pid_a} & {pid_b} permanently frozen!')

            action_type = 'triggered_resolution' if auto_resolve else 'none'
            write_status(
                blocked=2,
                wg=1.15,
                edges=4,
                risk=0.94,
                status='deadlock',
                action=action_type
            )

            # If auto_resolve is enabled, trigger automatic resolution after 12s
            if auto_resolve and elapsed >= 12.0:
                resolved = True
                append_log('RESOLVE', f'Auto-detector selected victim pid={pid_b} (cycle breaker)')
                append_log('RESOLVE', f'SIGKILL sent to victim pid={pid_b} — WFG cycle broken')
                append_log('RELEASE', f'resource=fork_1 released by kernel')
                append_log('HOLD', f'pid={pid_a} acquired resource=fork_1, thread resumed')
                break

            # If auto_resolve is disabled, remain in deadlock state until manual resolve!
            if not auto_resolve:
                # Every 4 seconds in deadlock, log that threads remain frozen
                if int(elapsed) % 4 == 0 and int(elapsed * 2) % 2 == 0:
                    append_log('DEADLOCK', f'Processes [{pid_a}, {pid_b}] remain locked in circular wait. Awaiting manual resolution...')

        time.sleep(0.4)

    # Wrap up resolution
    write_status(blocked=0, wg=0.0, edges=0, risk=0.05, status='running', action='none')
    append_log('MONITOR', f'System state restored: Normal execution active.')
    print("[demo_mock] [RESOLVED] System restored to normal state.\n")

def main():
    print("=" * 60)
    print("  AI-DeadlockGuard — Telemetry Generator")
    print("  Listening for GUI 'Create Deadlock' and 'Resolve' triggers...")
    print("=" * 60)

    # Initialize log
    with open(LOG_PATH, 'w', encoding='utf-8') as f:
        f.write(f"{datetime.now().strftime('[%H:%M:%S]')} MONITOR AI-DeadlockGuard daemon started.\n")

    t = 0
    while True:
        # Check if GUI requested a deadlock injection
        if os.path.exists(INJECT_TRIGGER_PATH):
            auto_resolve = True
            try:
                with open(INJECT_TRIGGER_PATH, 'r', encoding='utf-8') as f:
                    content = f.read().strip()
                    if content:
                        try:
                            cfg = json.loads(content)
                            auto_resolve = bool(cfg.get('auto_resolve', True))
                        except:
                            if 'false' in content.lower():
                                auto_resolve = False
                os.remove(INJECT_TRIGGER_PATH)
            except:
                pass
            run_injected_deadlock_sequence(auto_resolve=auto_resolve)
            t = 0
            continue

        # Check if GUI requested manual resolve
        if check_manual_resolve():
            append_log('RESOLVE', 'Manual resolution trigger cleared graph.')
            write_status(0, 0.0, 0, 0.04, 'running', 'none')

        # Normal background low-risk baseline
        blocked = 0 if random.random() > 0.2 else 1
        edges = random.randint(1, 4)
        wg = round(random.uniform(0.0, 0.08), 4)
        risk = round(random.uniform(0.04, 0.16), 4)

        write_status(blocked, wg, edges, risk, 'running', 'none')

        if int(t) % 4 == 0:
            res = random.choice(RESOURCES)
            p = random.randint(2100, 2199)
            evt = random.choice(['HOLD', 'RELEASE', 'HOLD'])
            append_log(evt, f'pid={p} resource={res}')

        t += 0.5
        time.sleep(0.5)

if __name__ == '__main__':
    main()
