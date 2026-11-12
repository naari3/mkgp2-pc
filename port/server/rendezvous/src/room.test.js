// node --test port/server/rendezvous/src/room.test.js
import test from "node:test";
import assert from "node:assert/strict";
import { RoomState, roomOf, STALE_MS } from "./room.js";

const q = (o) => new URLSearchParams(o);

test("numbers are given in order and kept", () => {
  const r = new RoomState();
  assert.equal(r.handle(q({ token: "aaaaaaaa" }), 0).text, "id 1\n");
  assert.equal(r.handle(q({ token: "bbbbbbbb" }), 10).text, "id 2\n");
  assert.equal(r.handle(q({ token: "aaaaaaaa" }), 20).text, "id 1\n");
});

test("addresses reach the others only", () => {
  const r = new RoomState();
  r.handle(q({ token: "aaaaaaaa", id: "1", ip: "203.0.113.5", port: "29564" }), 0);
  assert.equal(r.handle(q({ token: "bbbbbbbb", id: "2", ip: "198.51.100.7", port: "40001" }), 10).text,
    "id 2\npeer 1 203.0.113.5 29564\n");
  assert.equal(r.handle(q({ token: "aaaaaaaa", id: "1", ip: "203.0.113.5", port: "29564" }), 20).text,
    "id 1\npeer 2 198.51.100.7 40001\n");
});

test("a wanted number that is held is refused, a free one is given", () => {
  const r = new RoomState();
  r.handle(q({ token: "aaaaaaaa", id: "3" }), 0);
  const a = r.handle(q({ token: "bbbbbbbb", id: "3" }), 10);
  assert.equal(a.status, 409);
  assert.equal(a.text, "taken 3\n");
  assert.equal(r.handle(q({ token: "bbbbbbbb" }), 20).text, "id 1\n");
});

test("the fifth is told the room is full; silent members leave", () => {
  const r = new RoomState();
  for (const t of ["aaaaaaaa", "bbbbbbbb", "cccccccc", "dddddddd"]) {
    r.handle(q({ token: t }), 0);
  }
  assert.equal(r.handle(q({ token: "eeeeeeee" }), 10).text, "full\n");
  assert.equal(r.handle(q({ token: "eeeeeeee" }), STALE_MS + 1).text, "id 1\n");
});

test("bad input", () => {
  const r = new RoomState();
  assert.equal(r.handle(q({ token: "x" }), 0).status, 400);
  assert.equal(r.handle(q({ token: "aaaaaaaa", id: "5" }), 0).status, 400);
  assert.equal(r.handle(q({ token: "aaaaaaaa", ip: "1.2.3", port: "5" }), 0).status, 400);
  assert.equal(r.handle(q({ token: "aaaaaaaa", ip: "1.2.3.4", port: "0" }), 0).status, 400);
  assert.equal(roomOf("/v1/room/%E3%81%82abc"), "%E3%81%82abc");
  assert.equal(roomOf("/v1/room/a/b"), null);
});
