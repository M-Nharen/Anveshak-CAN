// One WebSocket to the backend, reconnecting by itself. The backend only sends what the open page
// needs, so the page name goes up whenever it changes (and again after a reconnect).
import { writable, get } from 'svelte/store';

export const snapshot = writable(null); // the latest {"t":"state"} message
export const connected = writable(false);
export const toasts = writable([]); // [{id, ok, text}]

const TOKEN_KEY = 'anv_token';
let ws = null;
let page = 'overview';
let nextReq = 1;
let nextToast = 1;
let retry = 500;

function readToken() {
  try {
    return sessionStorage.getItem(TOKEN_KEY);
  } catch {
    return null;
  }
}

export function toast(ok, text) {
  const id = nextToast++;
  toasts.update((t) => [...t.slice(-3), { id, ok, text }]);
  setTimeout(() => toasts.update((t) => t.filter((x) => x.id !== id)), ok ? 2500 : 5000);
}

export function send(msg) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false;
  ws.send(JSON.stringify(msg));
  return true;
}

// For commands that get an answer: shows the result as a toast.
export function command(msg, label) {
  const req = nextReq++;
  pending.set(req, label || msg.t);
  if (!send({ ...msg, req })) {
    pending.delete(req);
    toast(false, 'Not connected to the dashboard backend');
  }
}
const pending = new Map();

export function setPage(name) {
  page = name;
  send({ t: 'page', page: name });
}

export async function login(password) {
  const r = await fetch('/api/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ password }),
  });
  if (!r.ok) return false;
  const { token } = await r.json();
  try {
    sessionStorage.setItem(TOKEN_KEY, token);
  } catch {
    /* private mode: the login lasts until the page is reloaded */
  }
  send({ t: 'auth', token, req: 0 });
  return true;
}

function open() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => {
    retry = 500;
    connected.set(true);
    const token = readToken();
    if (token) send({ t: 'auth', token, req: 0 });
    send({ t: 'page', page });
  };
  ws.onmessage = (ev) => {
    let msg;
    try {
      msg = JSON.parse(ev.data);
    } catch {
      return;
    }
    if (msg.t === 'state') {
      snapshot.set(msg);
    } else if (msg.t === 'ack') {
      const label = pending.get(msg.req);
      pending.delete(msg.req);
      if (label) toast(msg.ok, msg.ok ? `${label}: ok` : `${label}: ${msg.msg}`);
      else if (!msg.ok && msg.msg) toast(false, msg.msg); // e.g. the drive message was refused
    }
  };
  ws.onclose = () => {
    connected.set(false);
    snapshot.update((s) => s); // keep showing the last data, greyed out via `connected`
    setTimeout(open, retry);
    retry = Math.min(retry * 2, 5000);
  };
  ws.onerror = () => ws.close();
}

export function start() {
  if (!ws) open();
}

export const isAuthed = () => get(snapshot)?.authed === true;
