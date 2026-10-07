# AI-DeadlockGuard

A Linux deadlock monitor with an AI early-warning model. It runs in WSL (or any Linux) as a
menu-driven C program, and its live output is shown both in the terminal and on a Flask website.

## What it shows

The same 5-process dining-philosophers workload is run two ways:

| | Run WITHOUT AI | Run WITH AI |
|---|---|---|
| Monitor | observes only | observes |
| AI model | off | scores deadlock risk every 100 ms |
| Result | the wait-for cycle closes and all 5 processes freeze in the kernel (`futex_wait`) until a watchdog kills them | risk crosses 0.75 before the cycle closes, the resolver SIGKILLs one victim, and the rest finish |

Menu option **3** runs both back to back and prints a side-by-side comparison.

## How it works

```
 philosopher processes (fork)          deadlock_guard (C, WSL)                       Flask (app.py)
 ───────────────────────────           ─────────────────────────────────            ───────────────
 lock/unlock robust mutexes  ──FIFO──▶ engine thread: wait-for graph, cycle          reads the JSON,
 in shared memory (mmap)               detection, features ──pipe──▶ ai_prediction.py  serves /stream
                                       ◀──risk── (RF+SVM+XGB)                        and static/ UI
                                       resolver: SIGKILL one victim
                                       writes /tmp/deadlock_guard_state.json  ──────▶ browser
                                       main thread: menu + terminal output    ◀────── /tmp/deadlock_guard.cmd
                                                                                      (run / resolve buttons)
```

- **C does the systems work**: `fork()`, process-shared robust mutexes in `mmap` memory, a named pipe
  (FIFO) for lock events, the wait-for graph and DFS cycle detection, `SIGKILL`-based resolution,
  `/proc/<pid>/wchan` inspection, two pthreads, and signal handling.
- **Python only does the ML**: `ai_prediction.py --for-c` runs as a persistent coprocess, so the
  model loads once. It reads `blocked,growth,edges,density` lines and answers each with a risk.
- **Flask only serves**: `app.py` reads the snapshot the C program writes every 100 ms, and passes
  button presses back through a command file. A run started from the website is streamed in the
  WSL terminal too.

## Project structure

```
Makefile              build ./deadlock_guard, plus setup / run / web targets
app.py                Flask server (API + dashboard)
ai_prediction.py      AI risk model: human report, or --for-c coprocess mode
requirements.txt      flask + pinned scikit-learn/xgboost
include/  src/
  main.c              menu (main thread)
  engine.c            monitor thread: runs, graph, AI, resolver, watchdog, JSON
  scenario.c          forked philosopher processes (robust shared mutexes)
  fifo_channel.c      worker → monitor named pipe
  ipc_reader.c        events → wait-for graph
  graph.c  cycle_detector.c  feature_extractor.c  resolver.c
  python_bridge.c     C ↔ Python coprocess (fork/pipe/dup2/exec)
  json_writer.c       atomic snapshot for Flask
  dashboard.c         terminal output (timeline, comparison, live box)
static/               web dashboard (index.html, style.css, script.js)
predictor/            dataset generation, training/tuning scripts, trained model
tests/                cycle-detector and model evaluation tests
logs/                 events.log, features.log, results.csv, predictor.log
```

## Running it in WSL

One-time setup (Ubuntu):

```bash
sudo apt install build-essential python3-venv
make setup        # venv in ~/.venvs/deadlock-guard with the pinned requirements
make
```

Then use two WSL terminals:

```bash
./deadlock_guard  # terminal 1: the menu-driven backend
make web          # terminal 2: the website → open http://localhost:8088 in Windows
```

```
=============================================
            AI DEADLOCK GUARD
=============================================
1. Run WITHOUT AI   (deadlock occurs)
2. Run WITH AI      (AI predicts and prevents it)
3. Compare both     (1 then 2, side by side)
4. Live Monitor
5. Last Results
6. Event Log
7. Exit
```

Ctrl+C leaves Live Monitor (or stops following a run) and returns to the menu. At the menu,
Ctrl+C exits.

To score features by hand: `~/.venvs/deadlock-guard/bin/python3 ai_prediction.py` (the latest live
features) or `... ai_prediction.py --features 4,2.1,9,0.0022`.

Settings: `DLG_WEB_PORT` (default 8088) changes the website port, and `DLG_PYTHON` points the C
program at a different Python interpreter.

## The AI model

A soft-voting ensemble (Random Forest + RBF SVM + XGBoost) trained on four wait-for-graph features:
blocked process count, wait-edge growth rate (smoothed over about 1 s), edge count and graph density.
`predictor/` holds the dataset generator and training/tuning scripts. `docs/model_evaluation_1000_tests.md`
has the evaluation. The model was trained on runs with 3–8 processes, which is why the demo
workload uses 5.
