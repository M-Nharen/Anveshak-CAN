<script>
  export let rows; // snapshot.firmware
</script>

<div class="card table-wrap">
  <h2>Firmware</h2>
  <p class="muted">
    Flashing is disabled: <code>anv-flash</code> (A3) hasn't landed yet. This page shows build IDs and
    flags mismatches within a node type in the meantime.
  </p>
  {#if rows}
    <table>
      <thead><tr><th>Node</th><th>Build ID</th><th>Status</th><th></th></tr></thead>
      <tbody>
        {#each rows as n (n.node_id)}
          <tr>
            <td>{n.name} <span class="muted">#{n.node_id}</span></td>
            <td>{n.fw_build_id === null ? '—' : '0x' + n.fw_build_id.toString(16).padStart(4, '0')}</td>
            <td>
              {#if !n.alive}
                <span class="pill grey">missing</span>
              {:else if n.mismatch}
                <span class="pill red">mismatch</span>
              {:else}
                <span class="pill green">ok</span>
              {/if}
            </td>
            <td><button disabled title="anv-flash (A3) not merged yet">Flash</button></td>
          </tr>
        {/each}
      </tbody>
    </table>
  {:else}
    <p class="muted">Waiting for the gateway…</p>
  {/if}
</div>
