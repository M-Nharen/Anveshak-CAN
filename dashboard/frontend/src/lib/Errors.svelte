<script>
  import { createEventDispatcher } from 'svelte';
  import { command } from './socket.js';

  export let data; // snapshot.errors
  export let authed = false;

  const dispatch = createEventDispatcher();

  const SEVERITY = ['info', 'warning', 'error', 'critical'];
  // An explicit locale sidesteps browsers/hosts with an unusual default locale tag.
  const fmtTime = (ns) => (ns ? new Date(ns / 1e6).toLocaleTimeString('en-US') : '—');
  const sevPill = (s) => (s >= 3 ? 'red' : s >= 2 ? 'red' : s >= 1 ? 'yellow' : 'grey');

  let roverLevel = 0;
  function setRoverLevel() {
    if (!authed) return dispatch('login');
    command({ t: 'set_debug', node_id: 0, level: Number(roverLevel), auto_off_min: 30 }, 'Whole-rover debug level');
  }
  function saveFlight() {
    if (!authed) return dispatch('login');
    command({ t: 'save_flight_recording', reason: 'manual' }, 'Save last minute');
  }

  const CATEGORY_NAMES = { 1: 'CAN', 2: 'Power', 3: 'Motor/driver', 4: 'Thermal', 5: 'Sensors', 6: 'System', 7: 'Boot', 8: 'Protocol', 9: 'Safety', 0x80: 'Host/gateway' };
</script>

{#if !data}
  <p class="muted">Waiting for the gateway…</p>
{:else}
  <div class="layout">
    <div class="card">
      <h2>Debug &amp; flight recorder</h2>
      <p>
        Whole-rover level: <b>{data.debug.level}</b>
        {#if data.debug.on}<span class="pill yellow">ON</span>{:else}<span class="pill grey">off</span>{/if}
        {#if data.debug.auto_off_s > 0}<span class="muted">· auto-off in {Math.round(data.debug.auto_off_s)}s</span>{/if}
      </p>
      <div class="row">
        <select bind:value={roverLevel}>
          <option value={0}>0 off</option>
          <option value={1}>1 normal</option>
          <option value={2}>2 debug</option>
          <option value={3}>3 trace</option>
        </select>
        <button on:click={setRoverLevel}>Apply to whole rover</button>
      </div>
      <p class="muted" style="margin-top: 10px">
        Flight recorder: {data.flight_recorder_enabled ? 'on' : 'off'} ·
        Gateway link: {data.link.configured ? (data.link.up ? 'up' : 'lost') : 'not configured'}
      </p>
      <button on:click={saveFlight}>Save last minute</button>
    </div>

    <div class="card">
      <h2>Active problems</h2>
      {#if data.active.length === 0}
        <p class="muted">None.</p>
      {:else}
        <table>
          <thead><tr><th>Node</th><th>Code</th><th>Severity</th><th>When</th><th>Cause</th></tr></thead>
          <tbody>
            {#each data.active as e (e.node_id + '-' + e.code)}
              <tr>
                <td>{e.node_id}</td>
                <td>{e.name} <span class="muted">0x{e.code.toString(16).padStart(4, '0')}</span></td>
                <td><span class="pill {sevPill(e.severity)}">{SEVERITY[e.severity] ?? e.severity}</span></td>
                <td>{fmtTime(e.time_ns)}</td>
                <td class="muted">{e.likely_cause || e.description || '—'}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    </div>

    <div class="card">
      <h2>Bus errors</h2>
      <table>
        <thead><tr><th>Type</th><th>Count</th><th>Likely cause</th></tr></thead>
        <tbody>
          {#each data.bus_errors as b (b.type)}
            <tr><td>{b.type.toUpperCase()}</td><td>{b.count}</td><td class="muted">{b.likely_cause}</td></tr>
          {/each}
        </tbody>
      </table>
    </div>

    <div class="card">
      <h2>Node × category counts</h2>
      {#if Object.keys(data.counts).length === 0}
        <p class="muted">No events yet.</p>
      {:else}
        <table>
          <thead><tr><th>Node</th><th>Category</th><th>Count</th></tr></thead>
          <tbody>
            {#each Object.entries(data.counts) as [node, byCategory] (node)}
              {#each Object.entries(byCategory) as [cat, count] (node + '-' + cat)}
                <tr><td>{node}</td><td>{CATEGORY_NAMES[cat] ?? `0x${Number(cat).toString(16)}`}</td><td>{count}</td></tr>
              {/each}
            {/each}
          </tbody>
        </table>
      {/if}
    </div>

    <div class="card table-wrap">
      <h2>Event list</h2>
      {#if data.events.length === 0}
        <p class="muted">No events yet.</p>
      {:else}
        <table>
          <thead><tr><th>When</th><th>Node</th><th>Type</th><th>Severity</th><th>Detail</th><th>Cleared</th></tr></thead>
          <tbody>
            {#each data.events as e (e.seq)}
              <tr>
                <td>{fmtTime(e.time_ns)}</td>
                <td>{e.node_id}</td>
                <td title={e.description}>{e.name}</td>
                <td>{e.code ? SEVERITY[e.severity] ?? e.severity : '—'}</td>
                <td>{e.detail}</td>
                <td>{e.cleared ? 'yes' : ''}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    </div>
  </div>
{/if}

<style>
  .layout { display: grid; gap: 14px; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); }
  .layout .table-wrap { grid-column: 1 / -1; }
</style>
