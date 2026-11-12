// The same rooms under plain Node, for tests without Cloudflare:
//   node port/server/rendezvous/src/local.js [port [address]]     (default 8787 127.0.0.1)
import http from "node:http";
import { RoomState, roomOf } from "./room.js";

const rooms = new Map();
const port = Number(process.argv[2] || 8787);
const host = process.argv[3] || "127.0.0.1";
http
  .createServer((req, res) => {
    const url = new URL(req.url, "http://localhost");
    const name = roomOf(url.pathname);
    if (req.method !== "GET" || name === null) {
      res.writeHead(404).end("mkgp2-pc rendezvous\n");
      return;
    }
    if (!rooms.has(name)) {
      rooms.set(name, new RoomState());
    }
    const answer = rooms.get(name).handle(url.searchParams, Date.now());
    res.writeHead(answer.status, { "content-type": "text/plain; charset=utf-8" }).end(answer.text);
  })
  .listen(port, host, () => console.log(`rendezvous on http://${host}:${port}`));
