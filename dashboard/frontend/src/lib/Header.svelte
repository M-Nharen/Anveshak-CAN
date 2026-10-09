<script>
  import { createEventDispatcher } from 'svelte';
  import { command } from './socket.js';

  export let snap;
  export let connected;

  const dispatch = createEventDispatcher();
  $: h = snap?.header;
  $: authed = snap?.authed === true;

  // Anything that moves hardware needs the login; ask for it instead of failing.
  function guarded(msg, label) {
    if (!authed) return dispatch('login');
    command(msg, label);
  }
  // E-stop is never behind a login and never asks for confirmation.
  function estop() {
    command({ t: 'estop' }, 'E-stop');
  }
</script>

<header class="card">
  <div class="row top">
    <div class="who">
      {#if !connected}
        <span class="pill red">backend offline</span>
      {:else if !h?.gateway}
        <span class="pill red">gateway not reachable</span>
      {:else if h.prompt}
        <span class="pill yellow">{h.prompt}</span>
      {:else}
        <span class="pill green">{h.leader_name ?? '?'} leads · term {h.term}</span>
      {/if}
      {#if h?.gateway}
        <span class="muted">this host: {h.host_name} ({h.role.toLowerCase()})</span>
      {/if}
    </div>

    <div class="row">
      <button on:click={() => guarded({ t: 'request_lead', host: 1 }, 'Make Orin lead')} disabled={!h?.gateway || h?.leader === 1}>Make Orin lead</button>
      <button on:click={() => guarded({ t: 'request_lead', host: 2 }, 'Make RPi lead')} disabled={!h?.gateway || h?.leader === 2}>Make RPi lead</button>
    </div>
  </div>

  <div class="row bottom">
    <span class="pill {h?.estop ? 'red' : 'green'}">{h?.estop ? 'E-STOP LATCHED' : 'E-stop clear'}</span>
    <span class="pill {h?.armed ? 'yellow' : 'grey'}">{h?.armed ? 'ARMED' : 'disarmed'}</span>
    <span class="pill {h?.control?.mine ? 'yellow' : 'grey'}" title="Who is sending drive targets">
      control: {h?.control?.mine ? 'this browser' : h?.control?.taken ? 'another browser' : 'none'}
    </span>
    <button on:click={() => guarded({ t: 'arm' }, 'Arm')} disabled={!h?.is_master}>Arm</button>
    <button on:click={() => guarded({ t: 'disarm' }, 'Disarm')} disabled={!h?.is_master}>Disarm</button>
    {#if h?.estop}
      <button on:click={() => guarded({ t: 'release_estop' }, 'Release E-stop')} disabled={!h?.is_master}>Release E-stop</button>
    {/if}
    <span class="spacer"></span>
    {#if authed}
      <span class="muted">logged in</span>
    {:else}
      <button on:click={() => dispatch('login')}>Log in</button>
    {/if}
    <button class="estop" on:click={estop}>E-STOP</button>
  </div>
</header>

<style>
  header { position: sticky; top: 0; z-index: 10; margin: 0 0 14px; display: grid; gap: 10px; }
  .top { justify-content: space-between; }
  .who { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
  .spacer { flex: 1; }
</style>
