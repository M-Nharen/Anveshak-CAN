<script>
  // Bench text from the touch screen: pick a node (or all), type up to 7 characters on the built-in
  // keyboard (the kiosk has no physical one), send. Nodes echo what they receive, so each sent line
  // shows which nodes really got it; text a node sends on its own (`msg` on its serial) shows too.
  import { command } from './socket.js';

  export let nodes = []; // bench.nodes
  export let messages = []; // bench.messages, newest first
  export let authed;

  const MAX = 7;
  const PRESET_KEY = 'anv_text_presets';
  const DEFAULT_PRESETS = ['hello', 'ping', 'led on', 'led off', 'test 1'];

  let target = 0; // 0 = every node
  let text = '';
  let shift = false;
  let presets = loadPresets();

  $: online = nodes.filter((n) => n.alive);
  $: if (target !== 0 && !online.some((n) => n.node_id === target)) target = 0;

  function loadPresets() {
    try {
      const saved = JSON.parse(localStorage.getItem(PRESET_KEY));
      if (Array.isArray(saved) && saved.every((p) => typeof p === 'string')) return saved;
    } catch {
      /* private mode or nothing saved: use the defaults */
    }
    return [...DEFAULT_PRESETS];
  }
  function savePresets() {
    try {
      localStorage.setItem(PRESET_KEY, JSON.stringify(presets));
    } catch {
      /* the presets then last until the page reloads */
    }
  }

  const rows = ['1234567890', 'qwertyuiop', 'asdfghjkl', 'zxcvbnm'];
  function key(ch) {
    if (text.length < MAX) text += shift ? ch.toUpperCase() : ch;
    shift = false;
  }
  function clean(v) {
    // Printable ASCII only, as the CAN frame carries it.
    return v.replace(/[^\x20-\x7e]/g, '').slice(0, MAX);
  }

  function send() {
    if (!text) return;
    command({ t: 'send_text', node_id: target, text }, `Send "${text}"`);
  }
  function savePreset() {
    if (text && !presets.includes(text)) {
      presets = [...presets, text].slice(-8);
      savePresets();
    }
  }
  function removePreset(p) {
    presets = presets.filter((x) => x !== p);
    savePresets();
  }

  const age = (s) => (s < 60 ? `${Math.round(s)}s` : `${Math.round(s / 60)}m`);
</script>

<div class="messages">
  <div class="compose card">
    <div class="targets">
      <span class="muted">To</span>
      <button class:primary={target === 0} on:click={() => (target = 0)}>All</button>
      {#each online as n (n.node_id)}
        <button class:primary={target === n.node_id} on:click={() => (target = n.node_id)}>{n.name}</button>
      {/each}
    </div>

    <div class="entry">
      <input
        bind:value={text}
        on:input={() => (text = clean(text))}
        on:keydown={(e) => e.key === 'Enter' && send()}
        maxlength={MAX}
        placeholder="type a message"
        spellcheck="false"
        autocomplete="off"
      />
      <span class="count muted">{text.length}/{MAX}</span>
      <button class="primary send" disabled={!authed || !text} on:click={send}>Send</button>
    </div>

    <div class="presets">
      {#each presets as p (p)}
        <span class="preset">
          <button on:click={() => (text = p)}>{p}</button><button class="x" title="Remove preset" on:click={() => removePreset(p)}>×</button>
        </span>
      {/each}
      <button class="save" disabled={!text || presets.includes(text)} on:click={savePreset}>+ save "{text || '…'}"</button>
    </div>

    <div class="keyboard">
      {#each rows as row, i}
        <div class="krow">
          {#if i === 3}<button class="wide" class:primary={shift} on:click={() => (shift = !shift)}>⇧</button>{/if}
          {#each row as ch}
            <button on:click={() => key(ch)}>{shift ? ch.toUpperCase() : ch}</button>
          {/each}
          {#if i === 3}<button class="wide" on:click={() => (text = text.slice(0, -1))}>⌫</button>{/if}
        </div>
      {/each}
      <div class="krow">
        <button on:click={() => key('-')}>-</button>
        <button on:click={() => key('!')}>!</button>
        <button class="space" on:click={() => key(' ')}>space</button>
        <button on:click={() => key('?')}>?</button>
        <button class="wide" on:click={() => (text = '')}>clear</button>
      </div>
    </div>
    {#if !authed}<p class="muted small">Not logged in: sending is disabled.</p>{/if}
  </div>

  <div class="log card">
    <h2>Messages</h2>
    {#each messages as m (m.key)}
      <div class="msg {m.dir}">
        {#if m.dir === 'tx'}
          <div class="line"><span class="arrow">→</span><b>{m.name}</b><span class="text">{m.text}</span><span class="age muted">{age(m.age_s)}</span></div>
          <div class="sub {m.received_by.length ? 'got' : 'muted'}">
            {m.received_by.length ? `✓ received by ${m.received_by.join(', ')}` : 'no reply yet'}
          </div>
        {:else}
          <div class="line"><span class="arrow">←</span><b>{m.name}</b><span class="text">{m.text}</span><span class="age muted">{age(m.age_s)}</span></div>
        {/if}
      </div>
    {:else}
      <p class="muted">Nothing sent yet. Pick a node, type, and press Send.</p>
    {/each}
  </div>
</div>

<style>
  .messages { display: grid; grid-template-columns: 1fr 340px; gap: 10px; height: 100%; min-height: 0; }
  .compose { display: grid; gap: 10px; align-content: start; overflow-y: auto; }
  .targets, .presets { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
  .targets button { min-height: 40px; min-width: 64px; }
  .entry { display: flex; gap: 8px; align-items: center; }
  .entry input {
    flex: 1; font: inherit; font-size: 22px; padding: 8px 12px; border-radius: 8px;
    border: 1px solid var(--line); background: var(--bg); color: var(--text); font-family: ui-monospace, monospace;
  }
  .count { font-variant-numeric: tabular-nums; }
  .send { min-height: 48px; padding: 0 22px; font-size: 17px; }
  .preset { display: inline-flex; }
  .preset button { min-height: 36px; border-radius: 8px 0 0 8px; }
  .preset .x { border-radius: 0 8px 8px 0; border-left: none; padding: 0 9px; color: var(--muted); }
  .save { min-height: 36px; color: var(--muted); }
  .keyboard { display: grid; gap: 5px; }
  .krow { display: flex; gap: 5px; justify-content: center; }
  .krow button { flex: 1; max-width: 52px; min-height: 42px; padding: 0; font-size: 17px; }
  .krow .wide { max-width: 80px; }
  .krow .space { max-width: 260px; flex: 4; }
  .small { font-size: 12px; margin: 0; }

  .log { overflow-y: auto; min-height: 0; display: flex; flex-direction: column; gap: 6px; }
  .msg { padding: 6px 0 6px 8px; border-left: 3px solid var(--accent); }
  .msg.rx { border-left-color: var(--ok); }
  .line { display: flex; gap: 8px; align-items: baseline; }
  .arrow { color: var(--muted); }
  .text { font-family: ui-monospace, monospace; font-size: 16px; flex: 1; overflow-wrap: anywhere; }
  .age { font-size: 12px; flex: none; }
  .sub { font-size: 12px; margin-top: 2px; }
  .sub.got { color: var(--ok); }

  @media (max-width: 760px) {
    .messages { grid-template-columns: 1fr; }
  }
</style>
