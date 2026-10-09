<script>
  import { command } from './socket.js';

  export let audit; // snapshot.audit: {recent, can_undo}

  const fmt = (v) => (typeof v === 'number' ? v.toFixed(2) : v ?? '—');
</script>

<div class="card">
  <div class="row" style="justify-content: space-between; margin-bottom: 8px">
    <h2 style="margin: 0">Audit log</h2>
    <button disabled={!audit?.can_undo} on:click={() => command({ t: 'undo_last_change' }, 'Undo last change')}>
      Undo last change
    </button>
  </div>
  {#if audit?.recent?.length}
    <table>
      <thead><tr><th>Kind</th><th>Who</th><th>Target</th><th>Old</th><th>New</th><th>Reason</th></tr></thead>
      <tbody>
        {#each audit.recent as e (e.id)}
          <tr>
            <td><span class="pill {e.kind === 'refused' ? 'red' : e.kind === 'undo' ? 'yellow' : 'green'}">{e.kind}</span></td>
            <td>{e.who}</td>
            <td>{e.target ?? '—'}{e.node !== undefined ? ` (node ${e.node})` : ''}</td>
            <td>{fmt(e.old)}</td>
            <td>{fmt(e.new ?? e.attempted)}</td>
            <td>{e.reason ?? '—'}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  {:else}
    <p class="muted">No control actions yet.</p>
  {/if}
</div>
