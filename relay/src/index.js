// Rubi relay: a tiny "post office" so a phone anywhere can see Rubi's camera without being on the home Wi-Fi.
//
//   Rubi camera  --wss-->  [ this Worker + Durable Object "Room" ]  <--wss--  phone page (web/xem.html)
//
// The room is chosen by the shared key: room = SHA-256("rubi-relay:" + key). Anyone with the same key (10+ letters/digits)
// lands in the same room, so the key must be long and random. Nothing is stored: frames are passed on and forgotten.
//
//   /ws?role=cam&k=KEY   the camera (one at a time, a new one replaces the old one)
//   /ws?role=view&k=KEY  a viewer (up to MAX_VIEWERS)
//
// Messages
//   room -> camera  (text)  "V<n>"   n = how many viewers are watching. The camera sends video only while n > 0.
//   room -> viewer  (text)  {"t":"s","cam":true|false,"n":<viewers>}   state, sent on every change
//   camera -> room  (binary) one JPEG frame; (text) short status, passed on to the viewers as it is
//   viewer -> room  (text)  short command like "q:2", passed on to the camera
//   camera/viewer -> room (text) "h" heartbeat; the room answers with the state (camera: "V<n>", viewer: the JSON above)
//
// Cost on the Cloudflare free plan: only INCOMING messages count (20 messages = 1 request), outgoing are free.
// The camera is silent while nobody watches, so an idle Rubi costs next to nothing.

import { DurableObject } from "cloudflare:workers";

const KEY_RE = /^[A-Za-z0-9]{10,40}$/;
const MAX_VIEWERS = 4;
const CMD_RE = /^[A-Za-z]:[A-Za-z0-9._-]{0,24}$/;
const OPEN = 1;

async function roomName(key) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode("rubi-relay:" + key));
  return [...new Uint8Array(d)].slice(0, 16).map((b) => b.toString(16).padStart(2, "0")).join("");
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === "/" || url.pathname === "/health") {
      return new Response("Rubi relay OK", { headers: { "content-type": "text/plain; charset=utf-8" } });
    }
    if (url.pathname !== "/ws") return new Response("not found", { status: 404 });
    if (req.headers.get("Upgrade") !== "websocket") return new Response("expected a websocket", { status: 426 });
    const role = url.searchParams.get("role");
    const key = url.searchParams.get("k") || "";
    if ((role !== "cam" && role !== "view") || !KEY_RE.test(key)) return new Response("bad request", { status: 400 });
    const stub = env.ROOM.get(env.ROOM.idFromName(await roomName(key)));
    return stub.fetch(req);
  },
};

export class Room extends DurableObject {
  #open(tag) {
    return this.ctx.getWebSockets(tag).filter((w) => w.readyState === OPEN);
  }

  #notify() {
    const cams = this.#open("cam");
    const views = this.#open("view");
    const state = JSON.stringify({ t: "s", cam: cams.length > 0, n: views.length });
    for (const v of views) {
      try { v.send(state); } catch (e) { /* closing */ }
    }
    for (const c of cams) {
      try { c.send("V" + views.length); } catch (e) { /* closing */ }
    }
  }

  async fetch(req) {
    const role = new URL(req.url).searchParams.get("role");
    if (role === "view" && this.#open("view").length >= MAX_VIEWERS) {
      return new Response("busy", { status: 429 });
    }
    if (role === "cam") {
      for (const old of this.ctx.getWebSockets("cam")) {
        try { old.close(1000, "replaced"); } catch (e) { /* already closed */ }
      }
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server, [role]);
    this.#notify();
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, msg) {
    const tags = this.ctx.getTags(ws);
    // Heartbeat "h" (every ~15 s from camera and viewers): answer with the current state, do not pass it on.
    // Both sides use the answer to notice a silently dead connection and reconnect.
    if (msg === "h") {
      try {
        if (tags.includes("cam")) ws.send("V" + this.#open("view").length);
        else ws.send(JSON.stringify({ t: "s", cam: this.#open("cam").length > 0, n: this.#open("view").length }));
      } catch (e) { /* closing */ }
      return;
    }
    if (tags.includes("cam")) {
      for (const v of this.#open("view")) {
        try { v.send(msg); } catch (e) { /* closing */ }
      }
    } else if (tags.includes("view")) {
      if (typeof msg === "string" && CMD_RE.test(msg)) {
        for (const c of this.#open("cam")) {
          try { c.send(msg); } catch (e) { /* closing */ }
        }
      }
    }
  }

  webSocketClose(ws, code, reason, wasClean) {
    try { ws.close(code && code !== 1005 && code !== 1006 ? code : 1000, "bye"); } catch (e) { /* already closed */ }
    this.#notify();
  }

  webSocketError(ws, err) {
    try { ws.close(1011, "error"); } catch (e) { /* already closed */ }
    this.#notify();
  }
}
