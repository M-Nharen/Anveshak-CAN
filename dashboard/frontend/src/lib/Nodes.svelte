<script>
  import { createEventDispatcher } from 'svelte';
  import { command } from './socket.js';

  export let rows; // snapshot.nodes
  export let authed = false;

  const dispatch = createEventDispatcher();
  const fmt = (v, unit = '') => (v === null || v === undefined ? '—' : `${v}${unit}`);
  const temps = (t) => Object.values(t ?? {}).length ? Object.entries(t).map(([k, v]) => `${k} ${v}°`).join(' · ') : '—';

  let edits = {}; // node_id -> chosen level, until applied

  function apply(node) {
    if (!authed) return dispatch('login');
    const level = edits[node.node_id] ?? node.debug_level;
    command({ t: 'set_debug', node_id: node.node_id, level: Number(level) }, `Debug level (${node.name})`);
    delete edits[node.node_id];
    edits = edits;
  }
</script>

{#if rows}
  <div class="card table-wrap">
    <table>
      <thead>
        <tr>
          <th>Node</th><th>Health</th><th>State</th><th>Heartbeat</th><th>Firmware</th><th>Uptime</th>
          <th>TEC/REC</th><th>Temperatures</th><th>Faults</th><th>Debug level</th>
        </tr>
      </thead>
      <tbody>
        {#each rows as n (n.node_id)}
          <tr>
            <td>{n.name} <span class="muted">#{n.node_id}</span></td>
            <td><span class="pill {n.health}" title={n.reasons.join(', ')}>{n.health}</span></td>
            <td>{n.state}</td>
            <td>{n.alive ? fmt(n.age_ms, ' ms') : 'missing'}</td>
            <td>{n.fw_build_id === null ? '—' : '0x' + n.fw_build_id.toString(16).padStart(4, '0')}</td>
            <td>{fmt(n.uptime_s, ' s')}{n.reboots ? ` (${n.reboots} reboot${n.reboots > 1 ? 's' : ''})` : ''}</td>
            <td>{n.tec}/{n.rec}</td>
            <td>{temps(n.temps)}</td>
            <td>{n.faults.length ? n.faults.join(', ') : '—'}</td>
            <td>
              <div class="row debug-cell">
                <select
                  value={edits[n.node_id] ?? n.debug_level}
                  on:change={(e) => (edits[n.node_id] = Number(e.currentTarget.value))}
                >
                  <option value={0}>0 off</option>
                  <option value={1}>1 normal</option>
                  <option value={2}>2 debug</option>
                  <option value={3}>3 trace</option>
                </select>
                <button on:click={() => apply(n)}>Set</button>
              </div>
            </td>
          </tr>
        {/each}
      </tbody>
    </table>
  </div>
{:else}
  <p class="muted">Waiting for the gateway…</p>
{/if}

<style>
  .debug-cell { gap: 6px; }
  .debug-cell select { font: inherit; padding: 4px 6px; border-radius: 6px; border: 1px solid var(--line); background: var(--bg); color: var(--text); }
  .debug-cell button { padding: 4px 10px; }
</style>
