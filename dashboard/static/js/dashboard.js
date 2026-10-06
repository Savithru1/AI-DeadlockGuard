/**
 * dashboard.js — Phase 14
 *
 * Vanilla JS live engine for AI-DeadlockGuard dashboard.
 * Four self-contained modules:
 *   1. ThemeManager  — dark/light toggle, persisted to localStorage
 *   2. RiskGauge     — animated SVG arc, colour transitions
 *   3. RiskChart     — scrolling Canvas area chart, no dependencies
 *   4. SSEClient     — EventSource('/stream') driving all live UI updates
 */

'use strict';

/* ═══════════════════════════════════════════════════════════════════════════
   1. THEME MANAGER
   ═══════════════════════════════════════════════════════════════════════════ */
const ThemeManager = (() => {
  const KEY = 'adg-theme';
  let current = 'dark';

  function apply(theme) {
    current = theme;
    document.documentElement.setAttribute('data-theme', theme);
    localStorage.setItem(KEY, theme);
    // Update toggle buttons
    document.querySelectorAll('.theme-btn').forEach(btn => {
      btn.classList.toggle('active', btn.dataset.theme === theme);
    });
  }

  function init() {
    const saved = localStorage.getItem(KEY) || 'dark';
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
   Includes animated needle tip bead and smooth numerical counter.
   ═══════════════════════════════════════════════════════════════════════════ */
const RiskGauge = (() => {
  let arcEl, needleEl, scoreEl, statusEl, statusTextEl;
  let currentScore = 0;
  let animFrameId = null;

  // Arc Center: (120, 115), Radius: 82, Span: 150° to 390° (240° total)
  const CX = 120;
  const CY = 115;
  const R  = 82;

  function _calcNeedle(pct) {
    const angle = 150 + pct * 240;
    const rad   = angle * (Math.PI / 180);
    return {
      x: CX + R * Math.cos(rad),
      y: CY + R * Math.sin(rad)
    };
  }

  function _colorForRisk(score) {
    if (score < 0.40) return '#10b981'; // emerald green
    if (score < 0.75) return '#f59e0b'; // amber warning
    return '#ef4444';                   // crimson danger
  }

  function _statusForRisk(score) {
    if (score < 0.40) return { label: 'SAFE', cls: 'safe' };
    if (score < 0.75) return { label: 'HIGH CONTENTION', cls: 'warning' };
    return { label: 'DEADLOCK DETECTED', cls: 'danger' };
  }

  function init() {
    arcEl        = document.getElementById('gaugeArc');
    needleEl     = document.getElementById('gaugeNeedle');
    scoreEl      = document.getElementById('gaugeScore');
    statusEl     = document.getElementById('gaugeStatus');
    statusTextEl = document.getElementById('gaugeStatusText');

    update(0);
  }

  function _animateScore(fromVal, toVal, color) {
    if (animFrameId) cancelAnimationFrame(animFrameId);
    if (!scoreEl) return;
    const start = performance.now();
    const duration = 250; // ms

    function step(now) {
      const progress = Math.min((now - start) / duration, 1);
      const val = Math.round(fromVal + (toVal - fromVal) * progress);
      scoreEl.textContent = val;
      scoreEl.style.color = color;
      const pctEl = scoreEl.parentElement?.querySelector('.gauge-percent');
      if (pctEl) pctEl.style.color = color;

      if (progress < 1) {
        animFrameId = requestAnimationFrame(step);
      }
    }
    animFrameId = requestAnimationFrame(step);
  }

  function update(score) {
    const clamped = Math.min(Math.max(score, 0), 1);
    const pct100  = clamped * 100;
    const color   = _colorForRisk(clamped);
    const status  = _statusForRisk(clamped);

    // 1. Arc stroke-dashoffset (pathLength="100")
    // When clamped = 0.08 (8%): offset = 92 (precisely 8% stroked from bottom-left!)
    const offset = Math.max(0, Math.min(100, 100 - pct100));
    if (arcEl) {
      arcEl.style.strokeDashoffset = offset.toFixed(2);
      arcEl.style.stroke = color;

      if (clamped >= 0.75) {
        arcEl.style.filter = 'drop-shadow(0 0 10px rgba(239, 68, 68, 0.75))';
      } else if (clamped >= 0.40) {
        arcEl.style.filter = 'drop-shadow(0 0 7px rgba(245, 158, 11, 0.55))';
      } else {
        arcEl.style.filter = 'drop-shadow(0 0 5px rgba(16, 185, 129, 0.4))';
      }
    }

    // 2. Needle tip bead coordinates
    if (needleEl) {
      const pos = _calcNeedle(clamped);
      needleEl.setAttribute('cx', pos.x.toFixed(2));
      needleEl.setAttribute('cy', pos.y.toFixed(2));
      needleEl.style.stroke = color;
      needleEl.style.filter = `drop-shadow(0 0 4px ${color})`;
    }

    // 3. Smooth numerical counter
    const oldScore = currentScore;
    currentScore = pct100;
    _animateScore(oldScore, pct100, color);

    // 4. Status badge
    if (statusEl && statusTextEl) {
      statusTextEl.textContent = status.label;
      statusEl.className = `gauge-status ${status.cls}`;
    }
  }

  return { init, update };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   3. RISK CHART  —  Canvas-based scrolling area chart
   Stores the last MAX_POINTS ticks and redraws on every update.
   ═══════════════════════════════════════════════════════════════════════════ */
const RiskChart = (() => {
  const MAX_POINTS = 60;  // 60 × 0.5s = 30 second window
  let canvas, ctx;
  let history = [];

  function _draw() {
    const W = canvas.width;
    const H = canvas.height;
    const theme = ThemeManager.get();

    ctx.clearRect(0, 0, W, H);

    // Background
    ctx.fillStyle = theme === 'dark' ? '#010409' : '#f0f2f4';
    ctx.fillRect(0, 0, W, H);

    // Grid lines (horizontal at 25%, 50%, 75%, 100%)
    const gridColor = theme === 'dark' ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.06)';
    const labelColor = theme === 'dark' ? '#4b5563' : '#9ca3af';
    ctx.strokeStyle = gridColor;
    ctx.lineWidth = 1;
    ctx.font = '10px system-ui, sans-serif';
    ctx.fillStyle = labelColor;
    ctx.textAlign = 'right';
    [0, 25, 50, 75, 100].forEach(pct => {
      const y = H - (pct / 100) * (H - 20) - 4;
      ctx.beginPath();
      ctx.moveTo(32, y);
      ctx.lineTo(W, y);
      ctx.stroke();
      ctx.fillText(`${pct}%`, 28, y + 4);
    });

    if (history.length < 2) return;

    const pts = history.slice(-MAX_POINTS);
    const stepX = (W - 32) / (MAX_POINTS - 1);

    const toXY = (i, val) => ({
      x: 32 + i * stepX,
      y: H - (val * (H - 20)) - 4
    });

    // Latest risk drives the gradient colour
    const latest = pts[pts.length - 1];
    let lineColor = '#10b981';
    if (latest >= 0.75) lineColor = '#ef4444';
    else if (latest >= 0.4) lineColor = '#f59e0b';

    // Area fill with vertical gradient
    const grad = ctx.createLinearGradient(0, 0, 0, H);
    grad.addColorStop(0, lineColor + 'aa');
    grad.addColorStop(1, lineColor + '08');

    ctx.beginPath();
    const first = toXY(0, pts[0]);
    ctx.moveTo(first.x, H - 4);
    ctx.lineTo(first.x, first.y);

    for (let i = 1; i < pts.length; i++) {
      const prev = toXY(i - 1, pts[i - 1]);
      const curr = toXY(i, pts[i]);
      const cpx  = (prev.x + curr.x) / 2;
      ctx.bezierCurveTo(cpx, prev.y, cpx, curr.y, curr.x, curr.y);
    }

    const last = toXY(pts.length - 1, pts[pts.length - 1]);
    ctx.lineTo(last.x, H - 4);
    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();

    // Line on top
    ctx.beginPath();
    ctx.moveTo(first.x, first.y);
    for (let i = 1; i < pts.length; i++) {
      const prev = toXY(i - 1, pts[i - 1]);
      const curr = toXY(i, pts[i]);
      const cpx  = (prev.x + curr.x) / 2;
      ctx.bezierCurveTo(cpx, prev.y, cpx, curr.y, curr.x, curr.y);
    }
    ctx.strokeStyle = lineColor;
    ctx.lineWidth   = 2.5;
    ctx.stroke();

    // Current value dot
    ctx.beginPath();
    ctx.arc(last.x, last.y, 4.5, 0, Math.PI * 2);
    ctx.fillStyle = lineColor;
    ctx.fill();
    ctx.strokeStyle = theme === 'dark' ? '#010409' : '#ffffff';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  function init() {
    canvas = document.getElementById('riskCanvas');
    ctx    = canvas.getContext('2d');

    function resize() {
      const rect = canvas.parentElement.getBoundingClientRect();
      canvas.width  = rect.width;
      canvas.height = 180;
      _draw();
    }
    window.addEventListener('resize', resize);
    resize();
  }

  function push(score) {
    history.push(score);
    if (history.length > MAX_POINTS * 2) history = history.slice(-MAX_POINTS);
    _draw();
  }

  // Redraw on theme change so colours update
  function redraw() { _draw(); }

  return { init, push, redraw };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   4. SSE CLIENT  —  drives all live UI updates
   ═══════════════════════════════════════════════════════════════════════════ */
const SSEClient = (() => {
  let evtSource  = null;
  let logEl      = null;
  let bannerEl   = null;
  let bannerTimer = null;
  let lastDeadlockState = false;

  const MAX_LOG_LINES = 200;

  function _el(id) { return document.getElementById(id); }

  function _updateStatCard(id, value, thresholds) {
    const el = _el(id);
    if (!el) return;
    el.textContent = value;
    // optional colour thresholds: { warn, danger }
    if (thresholds) {
      el.classList.remove('highlight', 'warn', 'danger');
      if      (value >= thresholds.danger) el.classList.add('danger');
      else if (value >= thresholds.warn)   el.classList.add('warn');
      else                                 el.classList.add('highlight');
    }
  }

  function _renderLog(lines) {
    if (!logEl) return;
    if (!lines || lines.length === 0) {
      logEl.innerHTML = '<div class="empty-log">Waiting for monitor events…</div>';
      return;
    }

    const fragment = document.createDocumentFragment();
    lines.slice(-MAX_LOG_LINES).forEach(line => {
      const div = document.createElement('div');
      div.className = 'log-entry';

      // Parse:  [HH:MM:SS] TYPE pid=X resource=Y
      const m = line.match(/^(\[[\d:]+\])\s+(\w+)\s+(.*)$/);
      if (m) {
        const [, ts, tag, rest] = m;
        const tagClass = {
          WAIT:    'tag-wait',
          HOLD:    'tag-hold',
          RELEASE: 'tag-release',
          DEADLOCK:'tag-deadlock',
          RESOLVE: 'tag-resolve',
        }[tag] || 'tag-monitor';
        div.innerHTML =
          `<span class="log-ts">${ts}</span>` +
          `<span class="log-tag ${tagClass}">${tag}</span> ${_esc(rest)}`;
      } else {
        div.textContent = line;
      }
      fragment.appendChild(div);
    });

    logEl.innerHTML = '';
    logEl.appendChild(fragment);
    logEl.scrollTop = logEl.scrollHeight;
  }

  function _esc(s) {
    return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
  }

  function _flashBanner(show, msg) {
    if (!bannerEl) return;
    if (show) {
      if (msg) bannerEl.innerHTML = `<span>⚠️</span><span>${msg}</span>`;
      bannerEl.classList.add('show');
      clearTimeout(bannerTimer);
    } else {
      bannerTimer = setTimeout(() => bannerEl.classList.remove('show'), 3000);
    }
  }

  function _updateStatus(state) {
    const badge   = _el('statusBadge');
    const monitor = state.monitor_status;
    const risk    = state.risk_score;

    if (!badge) return;
    badge.className = 'status-badge';
    if (monitor === 'deadlock' || risk >= 0.75) {
      badge.classList.add('deadlock');
      badge.querySelector('.status-dot').style.display = '';
      badge.querySelector('.status-text').textContent = 'DEADLOCK';
    } else if (monitor === 'running') {
      badge.classList.add('active');
      badge.querySelector('.status-text').textContent = 'MONITOR ACTIVE';
    } else {
      badge.classList.add('stopped');
      badge.querySelector('.status-text').textContent = 'STOPPED';
    }
  }

  function _onMessage(evt) {
    let state;
    try { state = JSON.parse(evt.data); } catch { return; }

    const risk = state.risk_score || 0;
    const f    = state.features   || {};

    // Gauge
    RiskGauge.update(risk);
    // Chart
    RiskChart.push(risk);

    // Stat cards
    _updateStatCard('valBlocked',  f.blocked_count   ?? 0, { warn: 2, danger: 4 });
    _updateStatCard('valEdges',    f.edge_count       ?? 0, { warn: 8, danger: 16 });
    _updateStatCard('valGrowth',   (f.wait_time_growth ?? 0).toFixed(3), null);
    _updateStatCard('valDensity',  (f.graph_density    ?? 0).toFixed(5), null);
    _updateStatCard('valRisk',     `${Math.round(risk * 100)}%`, null);

    // Log
    _renderLog(state.log_lines);

    // Status badge
    _updateStatus(state);

    // Deadlock banner
    const isDeadlock = state.monitor_status === 'deadlock' || risk >= 0.75;
    if (isDeadlock !== lastDeadlockState) {
      let bannerMsg = 'DEADLOCK DETECTED — Auto-resolution triggered';
      if (state.action !== 'triggered_resolution') {
        bannerMsg = 'DEADLOCK DETECTED — Auto-resolution DISABLED. Click "Resolve Deadlock" to break cycle.';
      }
      _flashBanner(isDeadlock, bannerMsg);
      lastDeadlockState = isDeadlock;
    }
  }

  function init() {
    logEl    = document.getElementById('logTerminal');
    bannerEl = document.getElementById('deadlockBanner');

    evtSource = new EventSource('/stream');
    evtSource.onmessage = _onMessage;
    evtSource.onerror   = () => {
      const badge = _el('statusBadge');
      if (badge) {
        badge.className = 'status-badge stopped';
        badge.querySelector('.status-text').textContent = 'DISCONNECTED';
      }
    };
  }

  return { init };
})();


/* ═══════════════════════════════════════════════════════════════════════════
   5. ACTION BUTTONS (CREATE DEADLOCK, FREEZE, & RESOLVE)
   ═══════════════════════════════════════════════════════════════════════════ */
function initActionButtons() {
  const createBtn        = document.getElementById('createDeadlockBtn');
  const createFreezeBtn  = document.getElementById('createFreezeDeadlockBtn');
  const resolveBtn       = document.getElementById('resolveBtn');
  const toggle           = document.getElementById('autoResolveToggle');
  const toggleStatusText = document.getElementById('autoResolveStatusText');
  const createBtnText    = document.getElementById('createBtnText');
  const feedback         = document.getElementById('actionFeedback');

  let activeTimer = null;

  function resetButtons() {
    if (activeTimer) clearInterval(activeTimer);
    activeTimer = null;

    if (createBtn) {
      createBtn.disabled = false;
      createBtn.classList.remove('running');
      const isAuto = toggle ? toggle.checked : true;
      createBtn.innerHTML = '<span>💥</span><span id="createBtnText">' + (isAuto ? 'Create Deadlock (Auto-Resolve)' : 'Create Deadlock (Auto-Resolve OFF)') + '</span>';
    }
    if (createFreezeBtn) {
      createFreezeBtn.disabled = false;
      createFreezeBtn.classList.remove('running');
      createFreezeBtn.innerHTML = '<span>🔒</span><span>Create Deadlock (No Auto-Resolve)</span>';
    }
    if (resolveBtn) {
      resolveBtn.disabled = false;
    }
  }

  // Toggle listener
  if (toggle) {
    toggle.addEventListener('change', () => {
      const isAuto = toggle.checked;
      if (toggleStatusText) {
        toggleStatusText.textContent = isAuto ? 'ENABLED' : 'DISABLED';
        toggleStatusText.className = 'setting-pill ' + (isAuto ? 'on' : 'off');
      }
      const curTextEl = document.getElementById('createBtnText');
      if (curTextEl) {
        curTextEl.textContent = isAuto ? 'Create Deadlock (Auto-Resolve)' : 'Create Deadlock (Auto-Resolve OFF)';
      }
      if (feedback) {
        feedback.textContent = isAuto
          ? 'Auto-resolution enabled: deadlock will be automatically broken after 12s.'
          : 'Auto-resolution disabled: deadlock will persist frozen until manually resolved.';
        feedback.className = 'resolve-feedback';
        setTimeout(() => { if (feedback.textContent.startsWith('Auto-resolution')) feedback.textContent = ''; }, 3500);
      }
    });
  }

  // Deadlock trigger function
  async function triggerDeadlock(autoResolve) {
    if (activeTimer) clearInterval(activeTimer);
    if (createBtn) createBtn.disabled = true;
    if (createFreezeBtn) createFreezeBtn.disabled = true;

    const targetBtn = autoResolve ? createBtn : createFreezeBtn;
    if (targetBtn) {
      targetBtn.classList.add('running');
      targetBtn.innerHTML = `<span>⏳</span><span>Injecting (${autoResolve ? 'Auto-Resolve' : 'Freeze'})...</span>`;
    }

    if (feedback) {
      feedback.textContent = autoResolve
        ? 'Injecting contention... Deadlock forming in 10-15s (will auto-resolve).'
        : 'Injecting contention... Deadlock forming in 10-15s (Auto-resolve DISABLED — will stay frozen).';
      feedback.className = 'resolve-feedback';
    }

    try {
      await fetch('/api/create-deadlock', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ auto_resolve: autoResolve })
      });
    } catch (e) {
      console.error('Failed to trigger deadlock creation:', e);
    }

    let remaining = 14;
    activeTimer = setInterval(() => {
      remaining -= 1;

      if (remaining > 6) {
        if (targetBtn) targetBtn.innerHTML = `<span>⏳</span><span>Contention Escalating (${remaining}s)...</span>`;
      } else if (remaining > 2) {
        if (autoResolve) {
          if (targetBtn) targetBtn.innerHTML = `<span>🚨</span><span>Deadlocked! Auto-resolving (${remaining}s)...</span>`;
        } else {
          if (targetBtn) targetBtn.innerHTML = `<span>🔒</span><span>Deadlocked! Frozen (Auto-Resolve OFF)...</span>`;
          if (feedback) {
            feedback.textContent = '⚠️ System frozen in deadlock! Click "⚡ Resolve Deadlock" to break cycle.';
            feedback.className = 'resolve-feedback err';
          }
        }
      } else if (remaining <= 0) {
        clearInterval(activeTimer);
        activeTimer = null;

        if (autoResolve) {
          resetButtons();
          if (feedback) {
            feedback.textContent = '✓ Deadlock auto-detected & broken successfully!';
            feedback.className = 'resolve-feedback ok';
            setTimeout(() => { feedback.textContent = ''; }, 4000);
          }
        } else {
          // Keep freeze button in frozen alert state until manual resolve is clicked!
          if (createFreezeBtn) {
            createFreezeBtn.innerHTML = '<span>🔒</span><span>Deadlocked (Awaiting Manual Resolve)</span>';
          }
          if (createBtn) createBtn.disabled = true;
          if (feedback) {
            feedback.textContent = '⚠️ Persistent Deadlock Active! Click "⚡ Resolve Deadlock" to restore system.';
            feedback.className = 'resolve-feedback err';
          }
        }
      }
    }, 1000);
  }

  // Button 1: Deadlock with Auto-Resolve (or follows toggle)
  if (createBtn) {
    createBtn.addEventListener('click', () => {
      const isAuto = toggle ? toggle.checked : true;
      triggerDeadlock(isAuto);
    });
  }

  // Button 2: Deadlock WITHOUT Auto-Resolve (Always Freeze)
  if (createFreezeBtn) {
    createFreezeBtn.addEventListener('click', () => {
      triggerDeadlock(false);
    });
  }

  // Button 3: Manual Resolve
  if (resolveBtn) {
    resolveBtn.addEventListener('click', async () => {
      resolveBtn.disabled = true;
      if (feedback) {
        feedback.textContent = 'Sending manual resolve signal (SIGKILL to victim)…';
        feedback.className = 'resolve-feedback';
      }
      try {
        const res  = await fetch('/api/resolve', { method: 'POST' });
        const data = await res.json();
        if (data.ok) {
          resetButtons();
          if (feedback) {
            feedback.textContent = '✓ Resolve signal sent! Circular wait broken and execution restored.';
            feedback.className = 'resolve-feedback ok';
            setTimeout(() => { if (feedback) feedback.textContent = ''; }, 4000);
          }
        } else if (feedback) {
          feedback.textContent = `✗ ${data.error}`;
          feedback.className = 'resolve-feedback err';
          setTimeout(() => { resolveBtn.disabled = false; }, 2000);
        }
      } catch (e) {
        if (feedback) {
          feedback.textContent = '✗ Network error.';
          feedback.className = 'resolve-feedback err';
        }
        setTimeout(() => { resolveBtn.disabled = false; }, 2000);
      }
    });
  }
}


/* ═══════════════════════════════════════════════════════════════════════════
   INIT
   ═══════════════════════════════════════════════════════════════════════════ */
document.addEventListener('DOMContentLoaded', () => {
  ThemeManager.init();
  RiskGauge.init();
  RiskChart.init();
  SSEClient.init();
  initActionButtons();

  // Redraw chart on theme change so gradient colours update
  document.querySelectorAll('.theme-btn').forEach(btn => {
    btn.addEventListener('click', () => setTimeout(RiskChart.redraw, 50));
  });
});
