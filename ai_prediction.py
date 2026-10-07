#!/usr/bin/env python3
"""
AI Deadlock Guard -- deadlock risk predictor.

The C program (deadlock_guard) does all the systems work: it forks the
worker processes, reads their lock events from the FIFO, maintains the
wait-for graph and extracts four live features from it. This script only
ever sees those four numbers and answers one question: how likely is it
that a deadlock is forming? It uses the Soft Voting Ensemble (Random
Forest + SVM + XGBoost) trained by predictor/tune_and_train.py.

Two output modes:
  (default)   A human-readable report, meant to be run by hand:
                  python3 ai_prediction.py                    # latest live features
                  python3 ai_prediction.py --features 4,2.1,9,0.0022
  --for-c     Coprocess mode used by the C program (src/python_bridge.c):
              load the model once, print READY, then answer every stdin line
              "blocked,growth,edges,density" with one line: the risk (0-1).
              If the model cannot be loaded it prints "ERROR <reason>".
"""

import argparse
import json
import os
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_PATH = os.path.join(BASE_DIR, "predictor", "models", "deadlock_predictor.pkl")
STATE_FILE = "/tmp/deadlock_guard_state.json"

FEATURES = ["blocked_count", "wait_time_growth", "edge_count", "graph_density"]
RISK_THRESHOLD = 0.75


def load_model():
    """Returns (model, pandas module). Imports are slow, so do them once."""
    import joblib
    import pandas as pd
    return joblib.load(MODEL_PATH), pd


def predict_risk(model, pd, values):
    frame = pd.DataFrame([values], columns=FEATURES)
    return float(model.predict_proba(frame)[0][1])


def serve_for_c():
    """Coprocess loop: one feature line in, one risk line out."""
    try:
        model, pd = load_model()
    except Exception as e:  # missing package, missing/incompatible model, ...
        print("ERROR {}: {}".format(type(e).__name__, e), flush=True)
        sys.exit(1)

    print("READY", flush=True)
    for line in sys.stdin:
        try:
            blocked, growth, edges, density = line.strip().split(",")
            values = [int(blocked), float(growth), int(edges), float(density)]
            print("{:.4f}".format(predict_risk(model, pd, values)), flush=True)
        except ValueError:
            print("nan", flush=True)


def read_live_features():
    if not os.path.exists(STATE_FILE):
        return None
    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            return json.load(f).get("features")
    except (OSError, json.JSONDecodeError):
        return None


def print_human_readable(values, risk, threshold, source):
    blocked, growth, edges, density = values
    print()
    print("--------------------------------------")
    print("AI DEADLOCK RISK PREDICTION")
    print("--------------------------------------")
    print("Features from    : {}".format(source))
    print("Blocked processes: {}".format(blocked))
    print("Wait growth      : {:.2f} new waits/s".format(growth))
    print("Graph edges      : {}".format(edges))
    print("Graph density    : {:.5f}".format(density))
    print("Risk             : {:.1f}%".format(risk * 100))
    print("Threshold        : {:.1f}%".format(threshold * 100))
    if risk >= threshold:
        print("Status           : DEADLOCK IMMINENT")
        print("⚠ The resolver would kill one victim now.")
    else:
        print("Status           : SAFE")
        print("✓ No intervention needed.")
    print("--------------------------------------")


def main():
    parser = argparse.ArgumentParser(description="AI Deadlock Guard risk predictor")
    parser.add_argument("--for-c", action="store_true",
                        help="coprocess mode for the C program (stdin/stdout protocol)")
    parser.add_argument("--features", metavar="B,G,E,D",
                        help="score these values instead of the live snapshot")
    parser.add_argument("--threshold", type=float, default=RISK_THRESHOLD)
    args = parser.parse_args()

    if args.for_c:
        serve_for_c()
        return

    if args.features:
        b, g, e, d = args.features.split(",")
        values, source = [int(b), float(g), int(e), float(d)], "command line"
    else:
        live = read_live_features()
        if live is None:
            print("No live data. Start the C program (./deadlock_guard) or pass --features.")
            sys.exit(1)
        values, source = [live[name] for name in FEATURES], STATE_FILE

    model, pd = load_model()
    print_human_readable(values, predict_risk(model, pd, values), args.threshold, source)


if __name__ == "__main__":
    main()
