<script>
  import { onMount } from 'svelte';
  import { snapshot, connected, toasts, start, setPage } from './lib/socket.js';
  import Header from './lib/Header.svelte';
  import Overview from './lib/Overview.svelte';
  import Nodes from './lib/Nodes.svelte';
  import Drive from './lib/Drive.svelte';
  import Bench from './lib/Bench.svelte';
  import Login from './lib/Login.svelte';

  const tabs = [
    ['overview', 'Overview'],
    ['nodes', 'Nodes'],
    ['drive', 'Drive'],
    ['bench', 'Bench'],
  ];

  // ?kiosk: the RPi's touch screen. Only the Bench page, full-screen, no tabs or header.
  const kiosk = new URLSearchParams(location.search).has('kiosk');
  let page = kiosk ? 'bench' : tabs.some(([id]) => `#${id}` === location.hash) ? location.hash.slice(1) : 'overview';
  let showLogin = false;

  function choose(id) {
    page = id;
    location.hash = id;
    setPage(id);
  }

  onMount(() => {
    start();
    setPage(page);
  });

  // The page name travels with every update, so ignore a stale snapshot for another page.
  $: ready = $snapshot && $snapshot.page === page;
</script>

{#if kiosk}
<main class="kiosk" class:stale={!$connected}>
  <Bench data={ready ? $snapshot.bench : null} header={$snapshot?.header} authed={$snapshot?.authed === true} connected={$connected} />
</main>
{:else}
<main>
  <Header snap={$snapshot} connected={$connected} on:login={() => (showLogin = true)} />

  <nav class="row" style="margin-bottom: 14px">
    {#each tabs as [id, label]}
      <button class:primary={page === id} on:click={() => choose(id)}>{label}</button>
    {/each}
  </nav>

  <section class:stale={!$connected}>
    {#if page === 'overview'}
      <Overview data={ready ? $snapshot.overview : null} />
    {:else if page === 'nodes'}
      <Nodes rows={ready ? $snapshot.nodes : null} />
    {:else if page === 'bench'}
      <Bench data={ready ? $snapshot.bench : null} header={$snapshot?.header} authed={$snapshot?.authed === true} connected={$connected} />
    {:else if page === 'drive'}
      <Drive
        data={ready ? $snapshot.drive : null}
        control={$snapshot?.header?.control}
        authed={$snapshot?.authed === true}
        connected={$connected}
      />
    {/if}
  </section>
</main>
{/if}

{#if showLogin}<Login on:done={() => (showLogin = false)} />{/if}

<div class="toasts">
  {#each $toasts as t (t.id)}
    <div class="toast {t.ok ? 'ok' : 'bad'}">{t.text}</div>
  {/each}
</div>

<style>
  .stale { opacity: 0.45; pointer-events: none; }
  .kiosk { max-width: none; height: 100vh; padding: 10px; overflow: hidden; }
  .kiosk.stale { pointer-events: auto; } /* E-STOP must stay pressable even with stale data */
  .toasts { position: fixed; right: 14px; bottom: 14px; display: grid; gap: 8px; z-index: 30; }
  .toast { padding: 9px 14px; border-radius: 8px; background: var(--panel); border: 1px solid var(--line); box-shadow: 0 4px 14px rgba(0, 0, 0, 0.18); }
  .toast.ok { border-left: 4px solid var(--ok); }
  .toast.bad { border-left: 4px solid var(--bad); }
</style>
