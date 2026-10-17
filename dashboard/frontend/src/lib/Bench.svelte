<script>
  // The RPi's 7" touch screen (1024x600): which nodes are on the bus, live, and what just changed.
  // Shown full-screen with ?kiosk in the URL, or as the Bench tab in the normal dashboard.
  import { command, toast } from './socket.js';
  import Messages from './Messages.svelte';

  export let data; // snapshot.bench
  export let header; // snapshot.header
  export let authed;
  export let connected;

  const SENSOR_LABELS = ['Temp', 'Heap', 'RX', 'TX'];
  const fmtSensor = (i, v) => (i === 0 ? `${(v / 100).toFixed(1)}°C` : i === 1 ? `${v} KiB` : `${v}`);
  const fmtUptime = (s) => {
    if (s === null || s === undefined) return '—';
    if (s < 60) return `${s}s`;
    if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
    return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
  };
  const macTail = (mac) => (mac ? mac.slice(-8) : '');

  // Activity dot: lit when a node's frame counter moved since the previous update (10 per second),
  // dark otherwise, so a talking node visibly flickers and a silent one stays dark.
  let lastRx = {};
  let lit = {};
  $: if (data) {
    const next = {};
    for (const n of data.nodes) next[n.node_id] = n.rx_count !== lastRx[n.node_id] && !lit[n.node_id];
    lit = next;
    lastRx = Object.fromEntries(data.nodes.map((n) => [n.node_id, n.rx_count]));
  }

  // A toast for every event that arrives after the screen opened (not the backlog at start-up).
  let seen = null;
  $: if (data) {
    const keys = data.events.map((e) => e.key);
    if (seen !== null) {
      for (const e of [...data.events].reverse()) if (!seen.has(e.key)) toast(e.kind !== 'bad', e.text);
    }
    seen = new Set([...(seen ?? []), ...keys]);
  }

  // Nodes or Messages, remembered per screen so a reload keeps the view.
  // ?view=messages in the URL wins (e.g. a kiosk that should open on Messages).
  let view = 'nodes';
  try {
    const asked = new URLSearchParams(location.search).get('view') ?? localStorage.getItem('anv_bench_view');
    view = asked === 'messages' ? 'messages' : 'nodes';
  } catch {
    /* default view */
  }
  function show(v) {
    view = v;
    try {
      localStorage.setItem('anv_bench_view', v);
    } catch {
      /* not remembered */
    }
  }

  // Text a node sent on its own gets a toast too, after the screen opened.
  let seenMsgs = null;
  $: if (data?.messages) {
    if (seenMsgs !== null) {
      for (const m of data.messages) if (m.dir === 'rx' && !seenMsgs.has(m.key)) toast(true, `${m.name}: ${m.text}`);
    }
    seenMsgs = new Set(data.messages.map((m) => m.key));
  }

  // The last text each node confirmed receiving, for its tile.
  $: lastGot = Object.fromEntries(
    (data?.nodes ?? []).map((n) => [n.node_id, (data.messages ?? []).find((m) => m.dir === 'tx' && m.received_by.includes(n.name))?.text]),
  );

  // Long-press a tile to free its ID in the RPi's table (for retiring or swapping a board).
  let pressTimer = null;
  let forgetting = null;
  function pressStart(n) {
    if (!n.mac) return; // only auto-addressed boards have a table entry
    pressTimer = setTimeout(() => (forgetting = n), 700);
  }
  function pressEnd() {
    clearTimeout(pressTimer);
  }
  function confirmForget() {
    command({ t: 'forget_node', node_id: forgetting.node_id }, `Forget ID ${forgetting.node_id}`);
    forgetting = null;
  }
</script>

<div class="bench">
  <div class="top card">
    <div class="count">
      {#if data}
        <span class="big">{data.online}</span>
        <span class="of">node{data.online === 1 ? '' : 's'} online{data.known > data.online ? ` · ${data.known - data.online} missing` : ''}</span>
      {:else}
        <span class="of">{connected ? 'Waiting for the gateway…' : 'Dashboard backend offline'}</span>
      {/if}
    </div>
    <div class="pills">
      {#if data}
        <span class="pill {data.bus_off ? 'red' : 'green'}">bus {data.bus_off ? 'OFF' : 'ok'} · {data.rx_fps} fps</span>
        {#if data.error_frames}<span class="pill yellow">{data.error_frames} error frames</span>{/if}
      {/if}
      {#if header?.gateway}
        <span class="pill {header.leader ? 'green' : 'grey'}">{header.leader ? `${header.leader_name ?? '#' + header.leader} leads` : 'no leader'}</span>
        {#if header.estop}<span class="pill red">E-STOP LATCHED</span>{/if}
      {/if}
    </div>
    <div class="views">
      <button class:primary={view === 'nodes'} on:click={() => show('nodes')}>Nodes</button>
      <button class:primary={view === 'messages'} on:click={() => show('messages')}>Messages</button>
    </div>
    <div class="actions">
      {#if header?.gateway && header.leader !== 2}
        <button class="primary" disabled={!authed} on:click={() => command({ t: 'request_lead', host: 2 }, 'Make RPi lead')}>Make RPi lead</button>
      {/if}
      {#if header?.estop && header?.is_master}
        <button disabled={!authed} on:click={() => command({ t: 'release_estop' }, 'Release E-stop')}>Release</button>
      {/if}
      <button class="estop" on:click={() => command({ t: 'estop' }, 'E-stop')}>E-STOP</button>
    </div>
  </div>

  {#if view === "messages"}
    <Messages nodes={data?.nodes ?? []} messages={data?.messages ?? []} {authed} />
  {:else}
  <div class="body">
    <div class="tiles">
      {#if data && data.nodes.length}
        {#each data.nodes as n (n.node_id)}
          <div
            class="tile card {n.health}"
            class:missing={!n.alive}
            on:pointerdown={() => pressStart(n)}
            on:pointerup={pressEnd}
            on:pointerleave={pressEnd}
            on:contextmenu|preventDefault
            role="button"
            tabindex="-1"
          >
            <div class="tile-head">
              <span class="dot" class:on={lit[n.node_id] && n.alive}></span>
              <span class="name">{n.name}</span>
              <span class="muted">#{n.node_id}</span>
              {#if n.hw}<span class="hw">{n.hw}</span>{/if}
            </div>
            <div class="state-row">
              <span class="pill {n.alive ? (n.state === 'DISARMED' || n.state === 'ARMED' ? 'green' : 'yellow') : 'grey'}">{n.alive ? n.state : 'MISSING'}</span>
              <span class="muted">{n.alive ? `up ${fmtUptime(n.uptime_s)}` : `last seen ${Math.round((n.age_ms ?? 0) / 1000)}s ago`}</span>
            </div>
            {#if n.sensors?.length}
              <div class="sensors">
                {#each n.sensors as v, i}
                  <div><span class="label">{SENSOR_LABELS[i]}</span><span class="value">{fmtSensor(i, v)}</span></div>
                {/each}
              </div>
            {/if}
            {#if lastGot[n.node_id]}<div class="got">Received: <code>{lastGot[n.node_id]}</code></div>{/if}
            <div class="foot muted">
              <span>TEC/REC {n.tec}/{n.rec}</span>
              {#if n.mac}<span>{macTail(n.mac)}</span>{/if}
            </div>
            {#if n.health !== 'green' && n.reasons.length}<div class="reasons">{n.reasons.join(' · ')}</div>{/if}
          </div>
        {/each}
      {:else if data}
        <div class="empty card">
          <p class="big-msg">No nodes yet</p>
          <p class="muted">Power up an ESP32 running the bench sketch. It asks for a node ID and shows up here within a second.</p>
          {#if !data.allocator}<p class="warn">This host is not handing out IDs right now (another host leads or has the lower tie-break).</p>{/if}
        </div>
      {/if}
    </div>

    <div class="log card">
      <h2>Events</h2>
      {#if data?.events.length}
        {#each data.events as e (e.key)}
          <div class="event {e.kind}"><span class="text">{e.text}</span><span class="age muted">{e.age_s < 60 ? `${Math.round(e.age_s)}s` : `${Math.round(e.age_s / 60)}m`}</span></div>
        {/each}
      {:else}
        <p class="muted">Nothing yet.</p>
      {/if}
      {#if !authed}<p class="muted small">Not logged in: buttons other than E-STOP are disabled.</p>{/if}
    </div>
  </div>
  {/if}
</div>

{#if forgetting}
  <div class="overlay">
    <div class="card dialog">
      <p>Free node ID <b>{forgetting.node_id}</b> ({forgetting.mac})?</p>
      <p class="muted">The board keeps using it until you type <code>forget</code> on its serial console.</p>
      <div class="row">
        <button class="primary" disabled={!authed} on:click={confirmForget}>Forget ID</button>
        <button on:click={() => (forgetting = null)}>Cancel</button>
      </div>
    </div>
  </div>
{/if}

<style>
  .bench { display: grid; grid-template-rows: auto 1fr; gap: 10px; height: 100%; min-height: 0; }
  .top { display: flex; align-items: center; gap: 14px; padding: 10px 14px; flex-wrap: wrap; }
  .count { display: flex; align-items: baseline; gap: 8px; }
  .big { font-size: 40px; font-weight: 700; line-height: 1; font-variant-numeric: tabular-nums; }
  .of { font-size: 18px; color: var(--muted); }
  .pills { display: flex; gap: 6px; flex-wrap: wrap; flex: 1; }
  .actions, .views { display: flex; gap: 8px; }
  .views button { min-height: 48px; padding: 0 16px; font-size: 16px; }
  .got { font-size: 13px; color: var(--ok); }
  .actions button { min-height: 48px; padding: 0 18px; font-size: 16px; }
  .actions .estop { min-width: 120px; }

  .body { display: grid; grid-template-columns: 1fr 270px; gap: 10px; min-height: 0; }
  .tiles { display: grid; gap: 10px; grid-template-columns: repeat(auto-fill, minmax(225px, 1fr)); align-content: start; overflow-y: auto; min-height: 0; }
  .tile { border-left: 5px solid var(--ok); display: grid; gap: 8px; user-select: none; touch-action: manipulation; }
  .tile.yellow { border-left-color: var(--warn); }
  .tile.red { border-left-color: var(--bad); }
  .tile.missing { opacity: 0.5; border-left-color: var(--grey); }
  .tile-head { display: flex; align-items: center; gap: 6px; }
  .name { font-weight: 700; font-size: 17px; }
  .hw { margin-left: auto; font-size: 12px; padding: 1px 7px; border-radius: 6px; background: var(--line); }
  .dot { width: 12px; height: 12px; border-radius: 50%; background: var(--line); flex: none; }
  .dot.on { background: var(--ok); box-shadow: 0 0 6px var(--ok); }
  .state-row { display: flex; gap: 8px; align-items: center; font-size: 13px; }
  .sensors { display: grid; grid-template-columns: 1fr 1fr; gap: 4px 10px; }
  .sensors div { display: flex; justify-content: space-between; font-variant-numeric: tabular-nums; }
  .sensors .label { color: var(--muted); font-size: 12px; }
  .foot { display: flex; gap: 10px; font-size: 12px; flex-wrap: wrap; font-variant-numeric: tabular-nums; }
  .reasons { font-size: 12px; color: var(--warn); }
  .tile.red .reasons { color: var(--bad); }
  .warn { color: var(--warn); }
  .empty { grid-column: 1 / -1; text-align: center; padding: 40px 20px; }
  .big-msg { font-size: 24px; font-weight: 600; margin: 0 0 8px; }

  .log { overflow-y: auto; min-height: 0; display: flex; flex-direction: column; gap: 6px; }
  .event { display: flex; justify-content: space-between; gap: 8px; font-size: 14px; padding: 5px 0 5px 8px; border-left: 3px solid var(--line); }
  .event.ok { border-left-color: var(--ok); }
  .event.warn { border-left-color: var(--warn); }
  .event.bad { border-left-color: var(--bad); }
  .age { flex: none; font-size: 12px; }
  .small { font-size: 12px; margin-top: auto; }

  .overlay { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.45); display: grid; place-items: center; z-index: 40; }
  .dialog { max-width: 420px; display: grid; gap: 6px; }
  .dialog button { min-height: 48px; padding: 0 22px; }

  @media (max-width: 760px) {
    .body { grid-template-columns: 1fr; }
  }
</style>
