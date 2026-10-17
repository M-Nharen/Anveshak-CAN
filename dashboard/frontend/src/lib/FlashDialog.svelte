<script>
  // Flash one node: pick the app image exported from the Arduino IDE (Sketch > Export Compiled Binary,
  // the ".ino.bin" file) and watch it go over CAN. The upload goes to the dashboard backend, which runs
  // anv-flash; progress comes back in every Bench update.
  import { createEventDispatcher } from 'svelte';
  import { command, readToken, toast } from './socket.js';

  export let node; // the tile's node
  export let flash; // bench.flash (null when nothing has been flashed since the backend started)
  export let authed;

  const dispatch = createEventDispatcher();
  let file = null;
  let uploading = false;

  $: mine = flash && flash.node_id === node.node_id;
  $: running = flash?.state === 'running';
  $: otherRunning = running && !mine;

  const PHASES = { starting: 'Starting', check: 'Checking', write: 'Writing', verify: 'Verifying', boot: 'Restarting', confirm: 'Waiting for the node', done: 'Done', error: 'Failed' };

  async function start() {
    if (!file) return;
    uploading = true;
    try {
      const token = readToken();
      const r = await fetch(`/api/flash/${node.node_id}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream', 'X-Filename': file.name, ...(token ? { Authorization: `Bearer ${token}` } : {}) },
        body: file,
      });
      const body = await r.json().catch(() => ({}));
      if (!r.ok) toast(false, `Flash: ${body.error ?? r.statusText}`);
    } catch (e) {
      toast(false, `Flash: upload failed (${e.message})`);
    } finally {
      uploading = false;
    }
  }
</script>

<div class="overlay">
  <div class="card dialog">
    <h2>Flash {node.name} <span class="muted">#{node.node_id}{node.hw ? ` · ${node.hw}` : ''}</span></h2>

    {#if mine}
      <div class="progress"><div class="bar {flash.state}" style="width: {flash.state === 'running' ? flash.pct : 100}%"></div></div>
      <p>
        <b>{PHASES[flash.phase] ?? flash.phase}</b>
        {#if flash.phase === 'write'}{Math.round(flash.pct)}%{/if}
        <span class="muted">· {flash.file} · {flash.elapsed_s}s</span>
      </p>
      {#if flash.msg}<p class="msg {flash.state}">{flash.msg}</p>{/if}
    {/if}

    {#if !running}
      <label class="pick">
        <input type="file" accept=".bin" on:change={(e) => (file = e.target.files[0] ?? null)} />
      </label>
      <p class="muted small">
        In the Arduino IDE: Sketch → Export Compiled Binary, then pick
        <code>build/…/bench_esp32_arduino.ino.bin</code> (not the <code>.merged.bin</code>). The board must be
        {node.hw || 'the right chip'} and disarmed. Takes about 30–60 s.
      </p>
    {:else if otherRunning}
      <p class="muted">Node {flash.node_id} is being flashed. One node at a time.</p>
    {/if}

    <div class="row">
      {#if running && mine}
        <button on:click={() => command({ t: 'flash_cancel' }, 'Cancel flash')}>Cancel</button>
      {:else if !running}
        <button class="primary" disabled={!authed || !file || uploading} on:click={start}>
          {uploading ? 'Uploading…' : mine ? 'Flash again' : 'Flash'}
        </button>
      {/if}
      <button on:click={() => dispatch('close')}>{running && mine ? 'Hide' : 'Close'}</button>
    </div>
    {#if !authed}<p class="muted small">Not logged in: flashing is disabled.</p>{/if}
  </div>
</div>

<style>
  .overlay { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.45); display: grid; place-items: center; z-index: 40; }
  .dialog { width: min(520px, 92vw); display: grid; gap: 10px; }
  .dialog button { min-height: 48px; padding: 0 22px; }
  .progress { height: 14px; border-radius: 7px; background: var(--line); overflow: hidden; }
  .bar { height: 100%; background: var(--accent); transition: width 0.3s; }
  .bar.done { background: var(--ok); }
  .bar.failed { background: var(--bad); }
  .msg.done { color: var(--ok); }
  .msg.failed { color: var(--bad); }
  .pick input { font: inherit; }
  .small { font-size: 12px; margin: 0; }
  p { margin: 0; }
</style>
