<script>
  import { onMount, onDestroy } from 'svelte';
  import { command, send } from './socket.js';

  export let data; // snapshot.drive
  export let control; // snapshot.header.control
  export let authed;
  export let connected;

  // --- input: keyboard and on-screen joystick. Hold-to-run: messages go out at 20 Hz only while
  // something is held; the backend zeroes the wheels 200 ms after the last one.
  const keys = new Set();
  let stick = { x: 0, y: 0 };
  let dragging = false;
  let timer = null;
  let wasActive = false;
  let fwd = 0;
  let turn = 0;

  function recompute() {
    const k = (names) => names.some((n) => keys.has(n));
    let f = (k(['w', 'arrowup']) ? 1 : 0) - (k(['s', 'arrowdown']) ? 1 : 0);
    let t = (k(['d', 'arrowright']) ? 1 : 0) - (k(['a', 'arrowleft']) ? 1 : 0);
    if (dragging) {
      f = -stick.y;
      t = stick.x;
    }
    fwd = f;
    turn = t;
  }

  function tick() {
    const active = fwd !== 0 || turn !== 0;
    if (control?.mine && (active || wasActive)) send({ t: 'drive', fwd, turn });
    wasActive = active;
  }

  function releaseAll() {
    keys.clear();
    dragging = false;
    stick = { x: 0, y: 0 };
    recompute();
  }

  function onKeyDown(e) {
    if (e.target instanceof HTMLElement && ['INPUT', 'TEXTAREA', 'SELECT'].includes(e.target.tagName)) return;
    const key = e.key.toLowerCase();
    if (key === ' ') {
      releaseAll();
      e.preventDefault();
      return;
    }
    if (['w', 'a', 's', 'd', 'arrowup', 'arrowdown', 'arrowleft', 'arrowright'].includes(key)) {
      keys.add(key);
      recompute();
      e.preventDefault();
    }
  }
  function onKeyUp(e) {
    keys.delete(e.key.toLowerCase());
    recompute();
  }
  function onVisibility() {
    if (document.hidden) releaseAll();
  }

  onMount(() => {
    timer = setInterval(tick, 50);
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('blur', releaseAll);
    document.addEventListener('visibilitychange', onVisibility);
  });
  onDestroy(() => {
    clearInterval(timer);
    releaseAll();
    window.removeEventListener('keydown', onKeyDown);
    window.removeEventListener('keyup', onKeyUp);
    window.removeEventListener('blur', releaseAll);
    document.removeEventListener('visibilitychange', onVisibility);
  });

  // --- joystick ---
  let pad;
  const R = 80; // pad radius in px
  function moveStick(e) {
    const r = pad.getBoundingClientRect();
    let x = (e.clientX - (r.left + r.width / 2)) / R;
    let y = (e.clientY - (r.top + r.height / 2)) / R;
    const len = Math.hypot(x, y);
    if (len > 1) {
      x /= len;
      y /= len;
    }
    stick = { x, y };
    recompute();
  }
  function padDown(e) {
    pad.setPointerCapture(e.pointerId);
    dragging = true;
    moveStick(e);
  }
  function padMove(e) {
    if (dragging) moveStick(e);
  }
  function padUp() {
    dragging = false;
    stick = { x: 0, y: 0 };
    recompute();
  }

  // --- limits: the slider shows the backend's value until the user drags it ---
  let capEdit = null;
  let accelEdit = null;
  $: cap = capEdit ?? data?.limits.cap ?? 0;
  $: accel = accelEdit ?? data?.limits.accel ?? 0;
  function commitCap() {
    command({ t: 'limits', cap: Number(cap) }, 'Speed cap');
    capEdit = null;
  }
  function commitAccel() {
    command({ t: 'limits', accel: Number(accel) }, 'Acceleration limit');
    accelEdit = null;
  }

  const fmt = (v) => (v === null || v === undefined ? '—' : v.toFixed(2));
  $: usable = connected && authed;
</script>

<div class="layout">
  <div class="card">
    <h2>Manual control</h2>
    <div class="row" style="margin-bottom: 12px">
      {#if control?.mine}
        <button on:click={() => command({ t: 'release_control' }, 'Release control')}>Release</button>
        <span class="pill yellow">you are driving</span>
      {:else}
        <button class="primary" disabled={!usable || control?.taken} on:click={() => command({ t: 'take_control' }, 'Take manual control')}>
          Take manual control
        </button>
        {#if control?.taken}<span class="pill grey">another browser has control</span>{/if}
        {#if !authed}<span class="muted">log in to drive</span>{/if}
      {/if}
    </div>

    <div class="pad-row">
      <div
        class="pad"
        class:enabled={control?.mine}
        bind:this={pad}
        on:pointerdown={padDown}
        on:pointermove={padMove}
        on:pointerup={padUp}
        on:pointercancel={padUp}
        role="presentation"
      >
        <div class="knob" style="transform: translate({stick.x * R}px, {stick.y * R}px)"></div>
      </div>
      <div class="hint">
        <div><b>W A S D</b> or arrow keys</div>
        <div class="muted">Hold to run. Release, or press space, to stop.</div>
        <div class="muted" style="margin-top: 8px">forward {fwd.toFixed(1)} · turn {turn.toFixed(1)}</div>
      </div>
    </div>

    <div class="row" style="margin: 14px 0 6px">
      <button disabled={!control?.mine} on:click={() => command({ t: 'speed_step', dir: -1 }, 'Speed −')}>Speed −</button>
      <button disabled={!control?.mine} on:click={() => command({ t: 'speed_step', dir: 1 }, 'Speed +')}>Speed +</button>
      <span>speed level <b>{data ? Math.round(data.limits.level * 100) : '—'}%</b></span>
    </div>

    <label class="slider">
      Speed cap <b>{Number(cap).toFixed(1)} rad/s</b>
      <input type="range" min="0" max={data?.limits.max_cap ?? 6} step="0.1" value={cap}
        disabled={!control?.mine} on:input={(e) => (capEdit = Number(e.currentTarget.value))} on:change={commitCap} />
    </label>
    <label class="slider">
      Acceleration limit <b>{Number(accel).toFixed(1)} rad/s²</b>
      <input type="range" min="0.5" max={data?.limits.max_accel ?? 20} step="0.5" value={accel}
        disabled={!control?.mine} on:input={(e) => (accelEdit = Number(e.currentTarget.value))} on:change={commitAccel} />
    </label>
  </div>

  <div class="card table-wrap">
    <h2>Wheels</h2>
    <table>
      <thead><tr><th>Drive</th><th>State</th><th>Target</th><th>Measured A</th><th>Measured B</th></tr></thead>
      <tbody>
        {#each data?.wheels ?? [] as w (w.drive)}
          <tr>
            <td>{w.drive}</td>
            <td><span class="pill {w.state === 'ARMED' ? 'yellow' : w.alive ? 'grey' : 'red'}">{w.alive ? w.state : 'missing'}</span></td>
            <td>{fmt(w.target)} rad/s</td>
            <td>{fmt(w.measured_a)}</td>
            <td>{fmt(w.measured_b)}</td>
          </tr>
        {/each}
      </tbody>
    </table>
    <p class="muted">The rover must be armed (header) before the wheels turn. Left side: drives 1–3, right side: 4–6.</p>
  </div>
</div>

<style>
  .layout { display: grid; gap: 14px; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); }
  .pad-row { display: flex; gap: 18px; align-items: center; flex-wrap: wrap; }
  .pad {
    width: 160px; height: 160px; border-radius: 50%;
    border: 2px solid var(--line); background: var(--bg);
    display: grid; place-items: center; touch-action: none; user-select: none; opacity: 0.5;
  }
  .pad.enabled { opacity: 1; border-color: var(--accent); }
  .knob { width: 54px; height: 54px; border-radius: 50%; background: var(--accent); }
  .slider { display: grid; gap: 4px; margin-top: 10px; }
</style>
