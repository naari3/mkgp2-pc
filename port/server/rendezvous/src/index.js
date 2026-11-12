// Rendezvous server of the mkgp2-pc cabinet link, as a Cloudflare Worker.
// It only tells the members of a room each other's public UDP address
// (docs/port/internet_play.md); no game data passes through it.
//
//   GET /v1/room/<name>?token=<hex>&id=<0-4>[&ip=<a.b.c.d>&port=<n>]
//
// One Durable Object per room name keeps the members in memory. When
// Cloudflare unloads an idle object the room is empty anyway; members that
// are still there name their number again with the next request.
import { DurableObject } from "cloudflare:workers";
import { RoomState, roomOf } from "./room.js";

const HEADERS = { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" };

export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.state = new RoomState();
  }

  async fetch(request) {
    const answer = this.state.handle(new URL(request.url).searchParams, Date.now());
    return new Response(answer.text, { status: answer.status, headers: HEADERS });
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const room = roomOf(url.pathname);
    if (request.method !== "GET" || room === null) {
      return new Response("mkgp2-pc rendezvous\n", { status: room === null ? 404 : 405, headers: HEADERS });
    }
    return env.ROOMS.get(env.ROOMS.idFromName(room)).fetch(request);
  },
};
