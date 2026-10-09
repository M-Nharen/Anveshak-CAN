<script>
  export let rows; // snapshot.nodes

  const fmt = (v, unit = '') => (v === null || v === undefined ? '—' : `${v}${unit}`);
  const temps = (t) => Object.values(t ?? {}).length ? Object.entries(t).map(([k, v]) => `${k} ${v}°`).join(' · ') : '—';
</script>

{#if rows}
  <div class="card table-wrap">
    <table>
      <thead>
        <tr>
          <th>Node</th><th>Health</th><th>State</th><th>Heartbeat</th><th>Firmware</th><th>Uptime</th>
          <th>TEC/REC</th><th>Temperatures</th><th>Faults</th>
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
          </tr>
        {/each}
      </tbody>
    </table>
  </div>
{:else}
  <p class="muted">Waiting for the gateway…</p>
{/if}
