// One room of the rendezvous server: who is in it, which cabinet number each
// has and at which public UDP address each can be reached. No Cloudflare
// API in here, so local.js can run the same code under Node for tests.
//
// A member is known by a token it makes up when it starts. It asks every
// two seconds; one that has not asked for STALE_MS is gone.

export const MAX_CABINETS = 4;
export const STALE_MS = 15000;

const IPV4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;
const TOKEN = /^[0-9a-f]{8,32}$/;

export class RoomState {
  constructor() {
    this.members = new Map(); // token -> {id, ip, port, seen}
  }

  // query: URLSearchParams with token, id (0 = any), ip, port (both optional).
  // Returns {status, text}; the text is the whole answer, one fact a line:
  //   id 2                      this member's cabinet number
  //   peer 1 203.0.113.5 29564  every other member that has told its address
  //   taken 2 / full / bad ...  instead of all of the above
  handle(query, now) {
    const token = query.get("token") || "";
    const want = Number(query.get("id") || "0");
    const ip = query.get("ip") || "";
    const port = Number(query.get("port") || "0");
    if (!TOKEN.test(token)) {
      return { status: 400, text: "bad token\n" };
    }
    if (!Number.isInteger(want) || want < 0 || want > MAX_CABINETS) {
      return { status: 400, text: "bad id\n" };
    }
    if (ip !== "" && (!IPV4.test(ip) || !Number.isInteger(port) || port < 1 || port > 65535)) {
      return { status: 400, text: "bad address\n" };
    }
    for (const [t, m] of this.members) {
      if (now - m.seen > STALE_MS) {
        this.members.delete(t);
      }
    }
    const taken = new Set();
    for (const [t, m] of this.members) {
      if (t !== token) {
        taken.add(m.id);
      }
    }
    let me = this.members.get(token);
    let id = want !== 0 ? want : me ? me.id : 0;
    if (id !== 0 && taken.has(id)) {
      if (want !== 0) {
        this.members.delete(token);
        return { status: 409, text: `taken ${id}\n` };
      }
      id = 0;
    }
    if (id === 0) {
      for (let k = 1; k <= MAX_CABINETS && id === 0; k++) {
        if (!taken.has(k)) {
          id = k;
        }
      }
      if (id === 0) {
        return { status: 409, text: "full\n" };
      }
    }
    me = { id, ip: ip !== "" ? ip : "", port: ip !== "" ? port : 0, seen: now };
    this.members.set(token, me);
    let text = `id ${id}\n`;
    for (const [t, m] of this.members) {
      if (t !== token && m.ip !== "") {
        text += `peer ${m.id} ${m.ip} ${m.port}\n`;
      }
    }
    return { status: 200, text };
  }

  get empty() {
    return this.members.size === 0;
  }
}

// "/v1/room/<name>" -> the name as it stands in the path (still
// percent-encoded: it is only ever compared), or null.
export function roomOf(pathname) {
  const m = /^\/v1\/room\/([A-Za-z0-9%._~-]{1,192})$/.exec(pathname);
  return m ? m[1] : null;
}
