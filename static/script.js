/**
 * script.js — AI-DeadlockGuard live dashboard.
 *
 * The page is served by Flask (app.py) inside WSL. Flask streams the C
 * backend's JSON snapshot over Server-Sent Events (/stream) every 300 ms;
 * this file renders it. Buttons POST to /api/run and /api/resolve, which
 * hand the request to the C program — the same run then also appears in
 * the WSL terminal.
 *
 * Modules:
 *   1. ThemeManager  — dark/light toggle, persisted to localStorage
 *   2. RiskGauge     — animated SVG arc, colour transitions
 *   3. RiskChart     — risk over the current run (Canvas, no dependencies)
 *   4. Panels        — run summary, processes, comparison, event log
 *   5. Controls      — run / resolve buttons
 *   6. Stream        — EventSource('/stream') driving everything
 */

'use strict';

const THRESHOLD = 0.75;

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

/* ═══════════════════════════════════════════════════════════════════════════
   1. THEME MANAGER
   ═══════════════════════════════════════════════════════════════════════════ */
const ThemeManager = (() => {
  const KEY = 'adg-theme';
  let current = 'dark';

  function apply(theme) {
    current = theme;
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem(KEY, theme); } catch { /* private mode */ }
    document.querySelectorAll('.theme-btn').forEach(btn => {
      btn.classList.toggle('active', btn.dataset.theme === theme);
    });
  }

  function init() {
    let saved = 'dark';
    try { saved = localStorage.getItem(KEY) || 'dark'; } catch { /* private mode */ }
    apply(saved);
    document.querySelectorAll('.theme-btn').forEach(btn => {
      btn.addEventListener('click', () => apply(btn.dataset.theme));
    });
  }

  return { init, apply, get: () => current };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   2. RISK GAUGE
   Mathematically exact 240° horseshoe arc (starts at 150°, sweeps to 390°).
   Uses pathLength="100" so stroke-dashoffset = 100 - (pct * 100) exactly.
   ═══════════════════════════════════════════════════════════════════════════ */
const RiskGauge = (() => {
  let arcEl, needleEl, scoreEl, statusEl, statusTextEl;
  let currentScore = 0;
  let animFrameId = null;

  const CX = 120;
  const CY = 115;
  const R  = 82;

  function _calcNeedle(pct) {
    const rad = (150 + pct * 240) * (Math.PI / 180);
    return { x: CX + R * Math.cos(rad), y: CY + R * Math.sin(rad) };
  }

  function _colorForRisk(score) {
    if (score < 0.40) return '#10b981';
    if (score < THRESHOLD) return '#f59e0b';
    return '#ef4444';
  }

  function _statusForRisk(score) {
    if (score < 0.40) return { label: 'SAFE', cls: 'safe' };
    if (score < THRESHOLD) return { label: 'RISK RISING', cls: 'warning' };
    return { label: 'DEADLOCK IMMINENT', cls: 'danger' };
  }

  function init() {
    arcEl        = document.getElementById('gaugeArc');
    needleEl     = document.getElementById('gaugeNeedle');
    scoreEl      = document.getElementById('gaugeScore');
    statusEl     = document.getElementById('gaugeStatus');
    statusTextEl = document.getElementById('gaugeStatusText');
    update(null);
  }

  function _animateScore(fromVal, toVal, color) {
    if (animFrameId) cancelAnimationFrame(animFrameId);
    const start = performance.now();
    const duration = 250;
    function step(now) {
      const progress = Math.min((now - start) / duration, 1);
      scoreEl.textContent = Math.round(fromVal + (toVal - fromVal) * progress);
      scoreEl.style.color = color;
      const pctEl = scoreEl.parentElement.querySelector('.gauge-percent');
      if (pctEl) pctEl.style.color = color;
      if (progress < 1) animFrameId = requestAnimationFrame(step);
    }
    animFrameId = requestAnimationFrame(step);
  }

  /** score: 0..1, or null when the AI is not running in this run. */
  function update(score, offLabel) {
    const off = score === null || score === undefined;
    const clamped = off ? 0 : Math.min(Math.max(score, 0), 1);
    const pct100  = clamped * 100;
    const color   = off ? '#6e7681' : _colorForRisk(clamped);

    arcEl.style.strokeDashoffset = (100 - pct100).toFixed(2);
    arcEl.style.stroke = color;
    if (off)                      arcEl.style.filter = 'none';
    else if (clamped >= THRESHOLD) arcEl.style.filter = 'drop-shadow(0 0 10px rgba(239, 68, 68, 0.75))';
    else if (clamped >= 0.40)      arcEl.style.filter = 'drop-shadow(0 0 7px rgba(245, 158, 11, 0.55))';
    else                           arcEl.style.filter = 'drop-shadow(0 0 5px rgba(16, 185, 129, 0.4))';

    const pos = _calcNeedle(clamped);
    needleEl.setAttribute('cx', pos.x.toFixed(2));
    needleEl.setAttribute('cy', pos.y.toFixed(2));
    needleEl.style.stroke = color;
    needleEl.style.filter = off ? 'none' : `drop-shadow(0 0 4px ${color})`;

    const oldScore = currentScore;
    currentScore = pct100;
    if (Math.round(oldScore) !== Math.round(pct100) || off) _animateScore(oldScore, pct100, color);

    const status = off ? { label: offLabel || 'AI OFF', cls: 'off' } : _statusForRisk(clamped);
    statusTextEl.textContent = status.label;
    statusEl.className = `gauge-status ${status.cls}`;
  }

  return { init, update };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   3. RISK CHART — risk over the current run, against run time in seconds
   ═══════════════════════════════════════════════════════════════════════════ */
const RiskChart = (() => {
  let canvas, ctx;
  let last = null;   // { history, elapsed, isAi, deadlockAt, killAt }

  const PAD_L = 36, PAD_R = 10, PAD_T = 10, PAD_B = 22;

  function _draw() {
    const W = canvas.width, H = canvas.height;
    const dark = ThemeManager.get() === 'dark';
    const plotW = W - PAD_L - PAD_R, plotH = H - PAD_T - PAD_B;

    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = dark ? '#010409' : '#f0f2f4';
    ctx.fillRect(0, 0, W, H);

    const s = last || { history: [], elapsed: 0, isAi: false };
    const span = Math.max(10, Math.ceil((s.elapsed || 0) + 0.5));
    const x = t => PAD_L + (t / span) * plotW;
    const y = v => PAD_T + (1 - v) * plotH;

    // Grid + labels
    ctx.font = '10px system-ui, sans-serif';
    ctx.lineWidth = 1;
    ctx.strokeStyle = dark ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.06)';
    ctx.fillStyle = dark ? '#6e7681' : '#9ca3af';
    ctx.textAlign = 'right';
    [0, 0.25, 0.5, 0.75, 1].forEach(v => {
      ctx.beginPath(); ctx.moveTo(PAD_L, y(v)); ctx.lineTo(W - PAD_R, y(v)); ctx.stroke();
      ctx.fillText(`${v * 100}%`, PAD_L - 6, y(v) + 3);
    });
    ctx.textAlign = 'center';
    const step = span <= 12 ? 1 : span <= 30 ? 5 : 10;
    for (let t = 0; t <= span; t += step) ctx.fillText(`${t}s`, x(t), H - 6);

    // Threshold line
    ctx.save();
    ctx.setLineDash([5, 5]);
    ctx.strokeStyle = 'rgba(239, 68, 68, 0.75)';
    ctx.beginPath(); ctx.moveTo(PAD_L, y(THRESHOLD)); ctx.lineTo(W - PAD_R, y(THRESHOLD)); ctx.stroke();
    ctx.restore();

    // Event markers
    function marker(t, color, label) {
      if (t === null || t === undefined) return;
      ctx.save();
      ctx.strokeStyle = color; ctx.fillStyle = color;
      ctx.setLineDash([2, 3]);
      ctx.beginPath(); ctx.moveTo(x(t), PAD_T); ctx.lineTo(x(t), PAD_T + plotH); ctx.stroke();
      ctx.textAlign = 'left';
      ctx.fillText(label, x(t) + 4, PAD_T + 10);
      ctx.restore();
    }
    marker(s.deadlockAt, '#ef4444', 'deadlock');
    marker(s.killAt, '#c084fc', s.isAi ? 'AI kill' : 'kill');

    if (!s.isAi) {
      ctx.fillStyle = dark ? '#8b949e' : '#636c76';
      ctx.textAlign = 'center';
      ctx.font = '12px system-ui, sans-serif';
      ctx.fillText(last ? 'AI not running in this run — no risk score' : 'No run yet', PAD_L + plotW / 2, PAD_T + plotH / 2);
      return;
    }
    const pts = s.history;
    if (pts.length < 2) return;

    const latest = pts[pts.length - 1][1];
    const lineColor = latest >= THRESHOLD ? '#ef4444' : latest >= 0.4 ? '#f59e0b' : '#10b981';

    const grad = ctx.createLinearGradient(0, PAD_T, 0, PAD_T + plotH);
    grad.addColorStop(0, lineColor + 'aa');
    grad.addColorStop(1, lineColor + '08');
    ctx.beginPath();
    ctx.moveTo(x(pts[0][0]), y(0));
    pts.forEach(([t, v]) => ctx.lineTo(x(t), y(v)));
    ctx.lineTo(x(pts[pts.length - 1][0]), y(0));
    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();

    ctx.beginPath();
    pts.forEach(([t, v], i) => (i ? ctx.lineTo(x(t), y(v)) : ctx.moveTo(x(t), y(v))));
    ctx.strokeStyle = lineColor;
    ctx.lineWidth = 2.5;
    ctx.stroke();

    const [lt, lv] = pts[pts.length - 1];
    ctx.beginPath();
    ctx.arc(x(lt), y(lv), 4.5, 0, Math.PI * 2);
    ctx.fillStyle = lineColor;
    ctx.fill();
    ctx.strokeStyle = dark ? '#010409' : '#ffffff';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  function init() {
    canvas = document.getElementById('riskCanvas');
    ctx = canvas.getContext('2d');
    function resize() {
      canvas.width = canvas.parentElement.getBoundingClientRect().width - 40;
      canvas.height = 190;
      _draw();
    }
    window.addEventListener('resize', resize);
    resize();
  }

  function update(state) {
    const run = state.run || {};
    const cur = (state.results || {}).current || {};
    if (!run.id) { last = null; _draw(); return; }
    last = {
      history: state.risk_history || [],
      elapsed: run.elapsed || 0,
      isAi: run.mode === 'ai',
      deadlockAt: cur.deadlock ? cur.deadlock_at : null,
      killAt: cur.resolved_at,
    };
    _draw();
  }

  return { init, update, redraw: _draw };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   4. PANELS
   ═══════════════════════════════════════════════════════════════════════════ */
const Panels = (() => {
  const STATE_LABEL = {
    starting: 'starting', holding: 'holding left', waiting: 'waiting',
    eating: 'eating', done: 'finished ✔', killed_ai: 'killed by AI',
    killed_manual: 'killed (manual)', killed_watchdog: 'killed (watchdog)',
  };
  let lastLogSeq = -1;
  let bannerShown = false;

  const el = id => document.getElementById(id);

  function stat(id, value, thresholds) {
    const node = el(id);
    node.textContent = value;
    if (thresholds) {
      node.classList.remove('highlight', 'warn', 'danger');
      if (value >= thresholds.danger)    node.classList.add('danger');
      else if (value >= thresholds.warn) node.classList.add('warn');
      else                               node.classList.add('highlight');
    }
  }

  function statusBadge(state) {
    const badge = el('statusBadge');
    const text = badge.querySelector('.status-text');
    const run = state.run || {};
    badge.className = 'status-badge';
    if (!state.backend_running) {
      badge.classList.add('stopped'); text.textContent = 'BACKEND OFFLINE';
    } else if (run.deadlocked) {
      badge.classList.add('deadlock'); text.textContent = 'DEADLOCK';
    } else if (run.phase === 'active') {
      badge.classList.add('active'); text.textContent = `RUNNING · AI ${run.mode === 'ai' ? 'ON' : 'OFF'}`;
    } else {
      badge.classList.add('idle'); text.textContent = 'BACKEND IDLE';
    }
  }

  function banner(state) {
    const run = state.run || {};
    const node = el('deadlockBanner');
    if (run.deadlocked) {
      el('bannerText').textContent = run.mode === 'ai'
        ? 'DEADLOCK — the AI is resolving it'
        : 'DEADLOCK — processes frozen. No AI in this run: the watchdog gives up after 6 s (or click Resolve)';
      node.classList.add('show');
      bannerShown = true;
    } else if (bannerShown) {
      bannerShown = false;
      setTimeout(() => node.classList.remove('show'), 1500);
    }
  }

  function runSummary(state) {
    const run = state.run || {};
    const cur = (state.results || {}).current;
    const node = el('runSummary');
    if (!run.id) { node.textContent = 'No run yet — use the buttons or the WSL menu'; return; }

    let chip = '';
    if (run.deadlocked) chip = '<span class="chip deadlock">deadlocked</span>';
    else if (run.phase === 'active') chip = '<span class="chip running">running</span>';
    else if (cur && cur.hung) chip = '<span class="chip hung">hung ✘</span>';
    else if (cur && (cur.timed_out || cur.finished === 0)) chip = '<span class="chip hung">failed ✘</span>';
    else if (cur) chip = '<span class="chip ok">recovered ✔</span>';

    node.innerHTML = `Run #${run.id} · ${run.mode === 'ai' ? 'WITH AI' : 'WITHOUT AI'} · ` +
      `${(run.elapsed || 0).toFixed(2)}s${run.compare_stage ? ' · comparison' : ''}` +
      ` · started from ${esc(run.origin || 'menu')}${chip}`;

    const cycle = el('cycleLine');
    if (run.cycle) { cycle.hidden = false; cycle.textContent = `Wait-for cycle: ${run.cycle}`; }
    else cycle.hidden = true;
  }

  function processes(state) {
    const grid = el('procGrid');
    const run = state.run || {};
    const workers = run.id ? (state.workers || []) : [];
    grid.innerHTML = workers.map(w => {
      const killed = w.state.startsWith('killed');
      const cls = [killed ? 'killed' : w.state, run.deadlocked && w.state === 'waiting' ? 'frozen' : ''].join(' ');
      const detail = killed || w.state === 'done' ? (w.pid ? `pid ${w.pid}` : '')
        : `${w.holds ? 'holds ' + w.holds : 'no forks'}${w.waiting_for ? ' · wants ' + w.waiting_for : ''}`;
      return `<div class="proc ${cls}">
        <div class="proc-name">${esc(w.name)}<span class="proc-pid">${w.pid ? 'pid ' + w.pid : ''}</span></div>
        <div class="proc-state">${esc(STATE_LABEL[w.state] || w.state)}</div>
        <div class="proc-detail" title="${esc(detail)}">${esc(detail)}</div>
      </div>`;
    }).join('');
  }

  function comparison(state) {
    const res = state.results || {};
    const base = res.baseline, ai = res.ai;
    const body = el('compareRows');
    const note = el('compareNote');
    if (!base && !ai) {
      body.innerHTML = '<tr><td colspan="3" class="muted">No completed runs yet.</td></tr>';
      note.textContent = '';
      return;
    }
    const total = (base || ai).total;
    const cell = (r, fn) => {
      if (!r) return '<td class="muted">—</td>';
      const [text, cls] = fn(r);
      return `<td class="${cls}">${esc(text)}</td>`;
    };
    const rows = [
      ['Deadlock formed', r => r.deadlock ? [`YES at +${r.deadlock_at.toFixed(2)}s`, 'bad']
                                          : [r.victims ? 'NO — prevented' : 'NO', 'good']],
      ['AI early warning', r => r.mode !== 'ai' ? ['not running', 'muted']
                              : r.ai_alert_at === null ? [`no alert (max ${(r.max_risk || 0).toFixed(2)})`, 'warn']
                              : [`+${r.ai_alert_at.toFixed(2)}s (risk ${r.ai_alert_risk.toFixed(2)})`, 'good']],
      ['Intervention', r => r.intervention === 'ai' ? [`AI killed ${r.victims}`, 'good']
                          : r.intervention === 'manual' ? [`manual: killed ${r.victims}`, 'warn']
                          : r.intervention === 'watchdog' ? ['none (watchdog)', 'bad'] : ['none', 'muted']],
      ['Time frozen', r => !r.deadlock ? ['0.00s', 'good']
                         : r.hung ? [`${r.frozen_for.toFixed(1)}s → ∞ (forced stop)`, 'bad']
                         : [`${r.frozen_for.toFixed(2)}s`, 'warn']],
      ['Philosophers finished', r => [`${r.finished} / ${total}`, r.finished ? 'good' : 'bad']],
      ['Processes killed', r => r.hung || r.timed_out ? [`${r.killed} (by watchdog)`, 'bad']
                              : r.killed && !r.victims ? [`${r.killed} (externally)`, 'bad']
                              : r.killed ? [`${r.killed} (victim)`, 'warn'] : ['0', 'good']],
      ['Run time', r => (r.hung || r.timed_out)
                          ? [`${r.duration.toFixed(2)}s (forced stop)`, 'bad']
                          : [`${r.duration.toFixed(2)}s (completed)`, 'good']],
      ['Outcome', r => r.hung ? ['HUNG ✘', 'bad']
                     : r.timed_out ? ['FAILED (timed out) ✘', 'bad']
                     : r.finished === 0 ? ['FAILED ✘', 'bad'] : ['RECOVERED ✔', 'good']],
    ];
    body.innerHTML = rows.map(([label, fn]) =>
      `<tr><td>${label}</td>${cell(base, fn)}${cell(ai, fn)}</tr>`).join('');

    if (base && ai && base.deadlock && ai.ai_alert_at !== null) {
      const lead = base.deadlock_at - ai.ai_alert_at;
      note.textContent = lead > 0
        ? `▶ The AI acted ${lead.toFixed(2)}s before the point where the unguarded run deadlocked — the cycle never closed.`
        : `▶ The AI acted ${(-lead).toFixed(2)}s after the cycle closed — it broke the deadlock instead of preventing it.`;
    } else {
      note.textContent = '';
    }
  }

  function eventLog(state) {
    const events = state.timeline || [];
    const newest = events.length ? events[events.length - 1].seq : 0;
    if (newest === lastLogSeq) return;
    lastLogSeq = newest;

    const node = el('logTerminal');
    if (!events.length) {
      node.innerHTML = '<div class="empty-log">Waiting for backend events…</div>';
      return;
    }
    const strong = new Set(['deadlock', 'alert', 'kill', 'watchdog', 'result', 'start']);
    node.innerHTML = events.map(e => {
      const tag = e.kind === 'start' ? 'RUN' : e.kind.toUpperCase();
      const cls = { hold: 'tag-hold', wait: 'tag-wait', deadlock: 'tag-deadlock' }[e.kind] || `tag-${e.kind}`;
      return `<div class="log-entry${strong.has(e.kind) ? ' strong' : ''}">` +
        `<span class="log-ts">#${e.run} ${e.t >= 0 ? '+' : ''}${e.t.toFixed(2)}s</span>` +
        `<span class="log-tag ${cls}">${tag}</span> ${esc(e.text)}</div>`;
    }).join('');
    node.scrollTop = node.scrollHeight;
  }

  function update(state) {
    statusBadge(state);
    el('offlineNotice').hidden = state.backend_running;
    if (!state.backend_running) {
      el('offlineText').textContent = state.error || 'Start the C program in WSL: ./deadlock_guard';
      return;
    }

    const run = state.run || {};
    const ai = state.ai || {};
    const f = state.features || {};

    const aiInRun = run.id && run.mode === 'ai';
    RiskGauge.update(aiInRun ? state.risk : null,
                     ai.status === 'unavailable' ? 'AI UNAVAILABLE' : 'AI OFF');
    el('aiModelLine').textContent = ai.status === 'ready'
      ? `AI model: ready · threshold ${Math.round((ai.threshold || THRESHOLD) * 100)}%`
      : `AI model: ${ai.status}${ai.message ? ' — ' + ai.message : ''}`;

    stat('valBlocked', f.blocked_count ?? 0, { warn: 2, danger: 4 });
    stat('valEdges', f.edge_count ?? 0, { warn: 6, danger: 9 });
    stat('valGrowth', (f.wait_time_growth ?? 0).toFixed(3), null);
    stat('valDensity', (f.graph_density ?? 0).toFixed(5), null);

    banner(state);
    runSummary(state);
    processes(state);
    RiskChart.update(state);
    comparison(state);
    eventLog(state);
  }

  return { update };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   5. CONTROLS
   ═══════════════════════════════════════════════════════════════════════════ */
const Controls = (() => {
  let runButtons, resolveBtn, feedback;
  let feedbackTimer = null;

  function say(text, cls) {
    feedback.textContent = text;
    feedback.className = `resolve-feedback ${cls || ''}`;
    clearTimeout(feedbackTimer);
    feedbackTimer = setTimeout(() => { feedback.textContent = ''; }, 5000);
  }

  async function post(url, body) {
    try {
      const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body || {}),
      });
      const data = await res.json();
      say(data.ok ? `✓ ${data.message}` : `✗ ${data.error}`, data.ok ? 'ok' : 'err');
    } catch {
      say('✗ Network error — is app.py running?', 'err');
    }
  }

  function init() {
    runButtons = document.querySelectorAll('[data-mode]');
    resolveBtn = document.getElementById('resolveBtn');
    feedback = document.getElementById('actionFeedback');

    runButtons.forEach(btn => btn.addEventListener('click', () => {
      btn.classList.add('running');
      post('/api/run', { mode: btn.dataset.mode });
    }));
    resolveBtn.addEventListener('click', () => post('/api/resolve'));
  }

  function update(state) {
    const run = state.run || {};
    const busy = run.phase === 'active' || run.pending || !!run.compare_stage;
    const aiOk = (state.ai || {}).status !== 'unavailable';
    runButtons.forEach(btn => {
      const needsAi = btn.dataset.mode !== 'baseline';
      btn.disabled = !state.backend_running || busy || (needsAi && !aiOk);
      const mine = busy && (btn.dataset.mode === (run.compare_stage ? 'compare' : run.mode));
      btn.classList.toggle('running', mine);
    });
    resolveBtn.disabled = !state.backend_running || run.phase !== 'active';
  }

  return { init, update };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   6. STREAM
   ═══════════════════════════════════════════════════════════════════════════ */
function startStream() {
  const source = new EventSource('/stream');
  source.onmessage = evt => {
    let state;
    try { state = JSON.parse(evt.data); } catch { return; }
    Panels.update(state);
    Controls.update(state);
  };
  source.onerror = () => {
    Panels.update({ backend_running: false, error: 'Lost connection to the Flask server (app.py).' });
    Controls.update({ backend_running: false });
  };
}

document.addEventListener('DOMContentLoaded', () => {
  ThemeManager.init();
  RiskGauge.init();
  RiskChart.init();
  Controls.init();
  startStream();

  document.querySelectorAll('.theme-btn').forEach(btn => {
    btn.addEventListener('click', () => setTimeout(RiskChart.redraw, 50));
  });
});
