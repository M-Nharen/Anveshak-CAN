<script>
  import { createEventDispatcher } from 'svelte';
  import { login, toast } from './socket.js';

  const dispatch = createEventDispatcher();
  let password = '';
  let busy = false;

  async function submit() {
    busy = true;
    const ok = await login(password).catch(() => false);
    busy = false;
    if (ok) {
      toast(true, 'Logged in');
      dispatch('done');
    } else {
      toast(false, 'Wrong password');
    }
  }
</script>

<div class="overlay" on:click|self={() => dispatch('done')} role="presentation">
  <form class="card" on:submit|preventDefault={submit}>
    <h2>Log in</h2>
    <p class="muted">Viewing needs no login. Driving, arming and choosing the leader do.</p>
    <div class="row">
      <input type="password" placeholder="Password" bind:value={password} autocomplete="current-password" />
      <button class="primary" type="submit" disabled={busy || !password}>Log in</button>
      <button type="button" on:click={() => dispatch('done')}>Cancel</button>
    </div>
  </form>
</div>

<style>
  .overlay { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.45); display: grid; place-items: center; z-index: 20; }
  form { width: min(420px, 92vw); }
</style>
