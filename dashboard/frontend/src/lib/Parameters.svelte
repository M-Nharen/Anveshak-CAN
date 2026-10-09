<script>
  import { onMount } from 'svelte';
  import { command, send } from './socket.js';
  import AuditPanel from './AuditPanel.svelte';

  export let data; // snapshot.parameters: {node_id, params}
  export let audit;

  const drives = [1, 2, 3, 4, 5, 6];
  let selected = 1;
  let edits = {}; // index -> value while editing

  function selectDrive(d) {
    selected = d;
    send({ t: 'select_param_node', node: 3 + d });
  }
  onMount(() => selectDrive(selected)); // pick drive 1 by default each time this page opens

  function valueFor(p) {
    return edits[p.index] ?? p.value ?? 0;
  }

  function setParam(p) {
    command({ t: 'set_param', node: data.node_id, index: p.index, value: Number(valueFor(p)) }, p.name);
    delete edits[p.index];
    edits = edits;
  }
  function saveParam(p) {
    command({ t: 'save_param', node: data.node_id, index: p.index }, `Save ${p.name} to flash`);
  }
  function restoreParam(p) {
    command({ t: 'restore_param', node: data.node_id, index: p.index }, `Restore ${p.name}`);
  }
</script>

<div class="layout">
  <div class="card table-wrap">
    <div class="row" style="margin-bottom: 10px">
      <h2 style="margin: 0 10px 0 0">Parameters — drive</h2>
      {#each drives as d}
        <button class:primary={selected === d} on:click={() => selectDrive(d)}>{d}</button>
      {/each}
    </div>
    {#if data?.params}
      <table>
        <thead><tr><th>Name</th><th>Description</th><th>Range</th><th>Value</th><th></th></tr></thead>
        <tbody>
          {#each data.params as p (p.index)}
            <tr>
              <td>{p.name}{p.unit ? ` (${p.unit})` : ''}</td>
              <td class="muted">{p.description}</td>
              <td>{p.minimum}–{p.maximum}</td>
              <td>
                <input type="number" min={p.minimum} max={p.maximum} step="any" disabled={p.locked}
                  value={valueFor(p)} on:input={(ev) => (edits = { ...edits, [p.index]: ev.currentTarget.value })} />
                {#if p.locked}<span class="pill grey" title="Disarm to change this parameter">locked</span>{/if}
                {#if !p.known}<span class="pill grey">unread</span>{/if}
              </td>
              <td class="row">
                <button disabled={p.locked} on:click={() => setParam(p)}>Set</button>
                <button disabled={p.locked} on:click={() => saveParam(p)}>Save</button>
                <button disabled={p.locked} on:click={() => restoreParam(p)}>Restore default</button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {:else}
      <p class="muted">Reading parameters…</p>
    {/if}
  </div>

  <AuditPanel {audit} />
</div>

<style>
  .layout { display: grid; gap: 14px; }
  input[type='number'] { width: 110px; font: inherit; padding: 5px 8px; border-radius: 6px; border: 1px solid var(--line); background: var(--bg); color: var(--text); }
</style>
