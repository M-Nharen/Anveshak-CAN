<script>
  import Chart from './Chart.svelte';

  export let data; // snapshot.overview

  // Last 60 s of bus load and frame rate, one sample per update.
  const MAX = 600;
  let times = [];
  let load = [];
  let fps = [];
  let t0 = null;

  $: if (data) {
    const now = performance.now() / 1000;
    if (t0 === null) t0 = now;
    times = [...times, now - t0].slice(-MAX);
    load = [...load, data.bus_load_pct].slice(-MAX);
    fps = [...fps, data.frames_per_s].slice(-MAX);
  }
</script>

{#if data}
  <div class="grid">
    <div class="card stat">
      <div class="label">Bus state</div>
      <div class="value" style="color: var(--{data.bus_state === 'ok' ? 'ok' : 'bad'})">{data.bus_state}</div>
    </div>
    <div class="card stat">
      <div class="label">Bus load</div>
      <div class="value">{data.bus_load_pct}%</div>
      <div class="sub">estimate from frame counts</div>
    </div>
    <div class="card stat">
      <div class="label">Frames / s</div>
      <div class="value">{data.frames_per_s}</div>
    </div>
    <div class="card stat">
      <div class="label">Error frames / s</div>
      <div class="value" style="color: var(--{data.error_frames_per_s > 0 ? 'bad' : 'text'})">{data.error_frames_per_s}</div>
    </div>
    <div class="card stat">
      <div class="label">Cycle overruns</div>
      <div class="value" style="color: var(--{data.overruns > 0 ? 'warn' : 'text'})">{data.overruns}</div>
      <div class="sub">worst lateness {data.worst_lateness_us} µs</div>
    </div>
    <div class="card stat">
      <div class="label">Send failures</div>
      <div class="value">{data.tx_failures}</div>
      <div class="sub">{data.undecoded_frames} undecoded frames</div>
    </div>
  </div>

  <div class="card">
    <h2>Bus, last minute</h2>
    <Chart
      {times}
      series={[
        { label: 'load %', values: load, color: '#2563eb' },
        { label: 'frames/s ÷ 10', values: fps.map((v) => v / 10), color: '#e0872b' },
      ]}
    />
  </div>

  <div class="card" style="margin-top: 14px">
    <h2>Hosts</h2>
    <table>
      <thead><tr><th>Host</th><th>Heard</th><th>Role</th><th>Term</th></tr></thead>
      <tbody>
        {#each data.peers as p}
          <tr><td>{p.name}</td><td>{p.seen ? 'yes' : 'no'}</td><td>{p.seen ? ['booting', 'standby', 'MASTER', 'observer'][p.role] : '—'}</td><td>{p.seen ? p.term : '—'}</td></tr>
        {/each}
      </tbody>
    </table>
  </div>
{:else}
  <p class="muted">Waiting for the gateway…</p>
{/if}
