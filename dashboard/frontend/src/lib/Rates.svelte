<script>
  import { command } from './socket.js';
  import AuditPanel from './AuditPanel.svelte';

  export let data; // snapshot.rates: {guard_pct, entries}
  export let audit;

  // Bits/s per frame and the bus bitrate match the backend's defaults (anv_dashboard/settings.py);
  // this is only a live preview while a selector moves -- the backend re-checks for real,
  // against its own measured bus load too, when the change is applied.
  const AVG_FRAME_BITS = 111;
  const CAN_BITRATE = 500_000;

  let edits = {}; // rate id -> value while the user is dragging its selector

  $: entries = data?.entries ?? [];

  function valueFor(e) {
    return edits[e.id] ?? e.hz;
  }

  function projectedPct(changedId) {
    let bits = 0;
    for (const e of entries) {
      const hz = e.id === changedId ? valueFor(e) : e.hz;
      bits += hz * frames(e) * AVG_FRAME_BITS;
    }
    return (bits / CAN_BITRATE) * 100;
  }

  // frames-per-occurrence isn't sent to the frontend (it's a backend modelling detail); a close
  // enough stand-in for the live preview: 7 for the control cycle, 1 for a heartbeat, 2 for a
  // node's feedback/telemetry rate (DRIVE_FB_A+B or DRIVE_TELEM_0+1), 1 for its own heartbeat.
  function frames(e) {
    if (e.id === 'cycle') return 7;
    if (e.id === 'host_hb') return 1;
    if (e.id.endsWith(':rate_hb_hz')) return 1;
    return 2;
  }

  function apply(e) {
    const hz = Number(valueFor(e));
    command({ t: 'set_rate', rate_id: e.id, hz }, e.label);
    delete edits[e.id];
    edits = edits;
  }
</script>

<div class="layout">
  <div class="card table-wrap">
    <div class="row" style="justify-content: space-between; margin-bottom: 8px">
      <h2 style="margin: 0">Rates</h2>
      <button on:click={() => command({ t: 'save_rate_defaults' }, 'Save as default')}>Save as default</button>
    </div>
    {#if data}
      <table>
        <thead><tr><th>Message</th><th>Source</th><th>Range</th><th>Rate</th><th>Projected load</th><th></th></tr></thead>
        <tbody>
          {#each entries as e (e.id)}
            <tr>
              <td>{e.label}</td>
              <td><span class="pill {e.source === 'host' ? 'yellow' : 'grey'}">{e.source}</span></td>
              <td>{e.floor_hz}–{e.ceil_hz} Hz</td>
              <td style="min-width: 220px">
                <input type="range" min={e.floor_hz} max={e.ceil_hz} step="1" value={valueFor(e)}
                  on:input={(ev) => (edits = { ...edits, [e.id]: Number(ev.currentTarget.value) })} />
                <b>{Number(valueFor(e)).toFixed(0)} Hz</b>
              </td>
              <td>
                <div class="bar"><div class="fill" class:over={projectedPct(e.id) > data.guard_pct} style="width: {Math.min(100, projectedPct(e.id))}%"></div></div>
                <span class="muted">{projectedPct(e.id).toFixed(1)}% (guard {data.guard_pct}%)</span>
              </td>
              <td><button disabled={valueFor(e) === e.hz} on:click={() => apply(e)}>Apply</button></td>
            </tr>
          {/each}
        </tbody>
      </table>
    {:else}
      <p class="muted">Waiting for the gateway…</p>
    {/if}
  </div>

  <AuditPanel {audit} />
</div>

<style>
  .layout { display: grid; gap: 14px; }
  .bar { width: 120px; height: 8px; border-radius: 4px; background: var(--line); overflow: hidden; }
  .fill { height: 100%; background: var(--ok); }
  .fill.over { background: var(--bad); }
</style>
