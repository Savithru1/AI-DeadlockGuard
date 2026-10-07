CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2 -g -Iinclude -pthread
SRC = src/main.c src/engine.c src/scenario.c src/fifo_channel.c src/ipc_reader.c \
      src/graph.c src/cycle_detector.c src/feature_extractor.c src/resolver.c \
      src/python_bridge.c src/json_writer.c src/dashboard.c
HDR = $(wildcard include/*.h)
OUT = deadlock_guard

# Python venv for the AI model + Flask (kept in the Linux home directory:
# venvs on /mnt/<drive> are slow). The C program finds it automatically.
VENV = $(HOME)/.venvs/deadlock-guard
PY   = $(VENV)/bin/python3

all: $(OUT)

$(OUT): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) -lm

# One-time: create the venv and install the pinned requirements.
setup:
	python3 -m venv $(VENV)
	$(PY) -m pip install --upgrade pip
	$(PY) -m pip install -r requirements.txt

# Terminal 1: the menu-driven backend.
run: $(OUT)
	./$(OUT)

# Terminal 2: the Flask website (http://localhost:8088).
web:
	$(PY) app.py

clean:
	rm -f $(OUT)
	rm -f /tmp/deadlock_guard_state.json /tmp/deadlock_guard_state.json.tmp \
	      /tmp/deadlock_guard.cmd /tmp/deadlock_guard.fifo

.PHONY: all setup run web clean
