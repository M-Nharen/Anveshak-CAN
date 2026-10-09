<script>
  import { onDestroy } from 'svelte';
  import { command, send } from './socket.js';
  import Chart from './Chart.svelte';

  export let data; // snapshot.test_bench: {drives, jog_drive, step}

  let drive = 1;
  let speed = 1.0;
  let holding = false;
  let kp = 10, ki = 1, kd = 0;
  let amplitude = 1.0;
  let duration = 2.0;

  function startJog() {
    send({ t: 'jog_start', drive, speed });
    holding = true;
  }
  function stopJog() {
    send({ t: 'jog_stop' });
    holding = false;
  }
  function setGains() {
    command({ t: 'set_gains', drive, kp: Number(kp), ki: Number(ki), kd: Number(kd) }, `Drive ${drive} gains`);
  }
  function startStep() {
    command({ t: 'step_test_start', drive, amplitude: Number(amplitude), duration_s: Number(duration) }, 'Step test');
  }
  onDestroy(stopJog);

  $: times = data?.step?.samples.map((s) => s[0]) ?? [];
  $: target = data?.step?.samples.map((s) => s[1]) ?? [];
  $: measured = data?.step?.samples.map((s) => (s[2] === null ? NaN : s[2])) ?? [];
</script>

<div class="layout">
  <div class="card">
    <h2>Jog</h2>
    <div class="row">
      <label>
        Drive
        <select bind:value={drive} disabled={holding}>
          {#each [1, 2, 3, 4, 5, 6] as d}<option value={d}>{d}</option>{/each}
        </select>
      </label>
      <label class="slider" style="flex: 1">
        Speed <b>{Number(speed).toFixed(1)} rad/s</b>
        <input type="range" min="-6" max="6" step="0.1" bind:value={speed} />
      </label>
      {#if !holding}
        <button class="primary" on:pointerdown={startJog} on:pointerup={stopJog} on:pointerleave={stopJog}>
          Hold to jog
        </button>
      {:else}
        <span class="pill yellow">jogging drive {drive}</span>
      {/if}
    </div>
    <p class="muted">The rover must be armed (header) before the wheel turns. Release stops it within 200 ms.</p>
  </div>

  <div class="card">
    <h2>PID gains (channel A)</h2>
    <div class="row">
      <label>Kp <input type="number" step="0.1" bind:value={kp} /></label>
      <label>Ki <input type="number" step="0.1" bind:value={ki} /></label>
      <label>Kd <input type="number" step="0.1" bind:value={kd} /></label>
      <button on:click={setGains}>Set gains</button>
    </div>
    <p class="muted">Gains are disarmed-only parameters: disarm first (header) to change them.</p>
  </div>

  <div class="card">
    <h2>Step response</h2>
    <div class="row" style="margin-bottom: 10px">
      <label>Amplitude <input type="number" step="0.1" bind:value={amplitude} /></label>
      <label>Duration (s) <input type="number" step="0.5" bind:value={duration} /></label>
      <button on:click={startStep}>Run step test</button>
    </div>
    {#if data?.step}
      <Chart
        {times}
        series={[
          { label: 'target', values: target, color: '#8a93a3' },
          { label: 'measured', values: measured, color: '#2563eb' },
        ]}
      />
    {:else}
      <p class="muted">No step test yet.</p>
    {/if}
  </div>

  <div class="card table-wrap">
    <h2>Drives</h2>
    <table>
      <thead><tr><th>Drive</th><th>State</th><th>Measured A</th></tr></thead>
      <tbody>
        {#each data?.drives ?? [] as w (w.drive)}
          <tr>
            <td>{w.drive}</td>
            <td><span class="pill {w.alive ? 'grey' : 'red'}">{w.alive ? w.state : 'missing'}</span></td>
            <td>{w.measured_a ?? '—'}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  </div>
</div>

<style>
  .layout { display: grid; gap: 14px; }
  .slider { display: grid; gap: 4px; }
</style>
