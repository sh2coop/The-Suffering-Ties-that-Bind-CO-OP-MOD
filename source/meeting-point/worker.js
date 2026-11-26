// The meeting point of the co-op mod: a host gets a short code (WOLF-4271), a friend who types it gets the host's
// internet address, and the host gets the friend's. Nothing else passes through here: the game's picture, sound and
// controls go straight from one PC to the other. A Cloudflare Worker with a D1 database (binding DB), free plan.
//
//   POST /host  {addr, secret, code?, lan?} -> {code}                  a new code, or the same one kept alive (every 5 min)
//   POST /wait  {code, secret}          -> {joiner: "ip:port"|null}  the host asks every few seconds: has someone typed it?
//   POST /join  {code, addr?}           -> {host: "ip:port", lan?}         or 404 {error: "not_found"}
//   POST /stop  {code, secret}          -> {ok: true}               the host closed the game or stopped inviting
// addr is "a.b.c.d:port", the game's port as seen from outside (the game finds it with STUN). A code stops working
// 15 minutes after the host's last sign of life; a friend's entry is kept for 2 minutes.

const WORDS = ("WOLF BEAR HAWK LION DEER CROW DOVE FROG GOAT LAMB MOLE MOTH NEWT PUMA SEAL SWAN TOAD WASP WREN LYNX " +
  "BOAT SHIP SAIL DOCK PIER REEF TIDE WAVE SAND DUNE HILL PEAK CAVE LAKE POND RAIN SNOW WIND MIST STAR MOON " +
  "FIRE IRON GOLD SALT ROPE NAIL BOLT GEAR LAMP BELL DRUM HARP HORN KITE COIN RING GATE WALL ROOF DOOR PATH ROAD " +
  "BARN MILL FARM TOWN PARK YARD TREE LEAF ROOT SEED MOSS FERN PINE PALM ROSE CORN RICE BEAN PLUM PEAR LIME KIWI " +
  "CAKE MILK SOUP TACO BLUE GRAY PINK RUBY JADE OPAL ONYX EAST WEST").split(" ").filter((w) => w.length === 4);

const HOST_SECONDS = 15 * 60, JOIN_SECONDS = 120;
const isAddr = (s) => typeof s === "string" && /^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$/.test(s);
const isCode = (s) => typeof s === "string" && /^[A-Z]{4}-\d{4}$/.test(s);
const isSecret = (s) => typeof s === "string" && /^[0-9a-f]{16,64}$/.test(s);
const reply = (o, status = 200) =>
  new Response(JSON.stringify(o), { status, headers: { "content-type": "application/json", "cache-control": "no-store" } });

let ready = false;
async function tables(db) {
  if (ready) return;
  await db.batch([
    db.prepare("CREATE TABLE IF NOT EXISTS hosts2 (code TEXT PRIMARY KEY, addr TEXT NOT NULL, secret TEXT NOT NULL, seen INTEGER NOT NULL, lan TEXT, ip TEXT)"),
    db.prepare("CREATE TABLE IF NOT EXISTS joins (code TEXT PRIMARY KEY, addr TEXT NOT NULL, seen INTEGER NOT NULL)"),
  ]);
  ready = true;
}

function newCode() {
  const r = new Uint32Array(2);
  crypto.getRandomValues(r);
  return WORDS[r[0] % WORDS.length] + "-" + String(r[1] % 10000).padStart(4, "0");
}

export default {
  async fetch(request, env) {
    const path = new URL(request.url).pathname;
    if (request.method === "GET" && path === "/") return reply({ ok: true, what: "co-op meeting point", version: 1 });
    if (request.method !== "POST") return reply({ error: "method" }, 405);
    let b;
    try {
      const text = await request.text();
      if (text.length > 400) return reply({ error: "too_long" }, 400);
      b = JSON.parse(text);
    } catch {
      return reply({ error: "body" }, 400);
    }
    const db = env.DB;
    // where the request comes from: two players behind one router (or two games on one PC) share it, and then the
    // friend is given the host's address on their own network, since most routers do not turn such traffic round
    const from = request.headers.get("CF-Connecting-IP") || "";
    await tables(db);
    const now = Math.floor(Date.now() / 1000);

    if (path === "/host") {
      if (!isAddr(b.addr) || !isSecret(b.secret) || (b.lan !== undefined && !isAddr(b.lan))) return reply({ error: "fields" }, 400);
      const lan = b.lan || null;
      if (isCode(b.code)) {  // the same code kept alive (and its address brought up to date)
        const kept = await db.prepare("UPDATE hosts2 SET addr = ?, seen = ?, lan = ?, ip = ? WHERE code = ? AND secret = ?").bind(b.addr, now, lan, from, b.code, b.secret).run();
        if (kept.meta.changes) return reply({ code: b.code });
      }
      await db.batch([
        db.prepare("DELETE FROM hosts2 WHERE seen < ?").bind(now - HOST_SECONDS),
        db.prepare("DELETE FROM joins WHERE seen < ?").bind(now - JOIN_SECONDS),
      ]);
      for (let tries = 0; tries < 12; tries++) {
        const code = newCode();
        const made = await db.prepare("INSERT OR IGNORE INTO hosts2 (code, addr, secret, seen, lan, ip) VALUES (?, ?, ?, ?, ?, ?)").bind(code, b.addr, b.secret, now, lan, from).run();
        if (made.meta.changes) return reply({ code });
      }
      return reply({ error: "busy" }, 503);
    }

    if (path === "/wait") {
      if (!isCode(b.code) || !isSecret(b.secret)) return reply({ error: "fields" }, 400);
      const row = await db
        .prepare("SELECT j.addr AS joiner, h.code AS code FROM hosts2 h LEFT JOIN joins j ON j.code = h.code AND j.seen >= ? WHERE h.code = ? AND h.secret = ?")
        .bind(now - JOIN_SECONDS, b.code, b.secret)
        .first();
      if (!row) return reply({ error: "not_found" }, 404);  // (the code ran out: the host asks for a new one)
      return reply({ joiner: row.joiner || null });
    }

    if (path === "/join") {
      // (asked twice: first without addr, to learn where the host is; then with this PC's own outside address,
      // which its game only knows once it has sent something to the host's)
      if (!isCode(b.code) || (b.addr !== undefined && !isAddr(b.addr))) return reply({ error: "fields" }, 400);
      const host = await db.prepare("SELECT addr, lan, ip FROM hosts2 WHERE code = ? AND seen >= ?").bind(b.code, now - HOST_SECONDS).first();
      if (!host) return reply({ error: "not_found" }, 404);
      if (b.addr !== undefined)
        await db.prepare("INSERT INTO joins (code, addr, seen) VALUES (?, ?, ?) ON CONFLICT(code) DO UPDATE SET addr = excluded.addr, seen = excluded.seen").bind(b.code, b.addr, now).run();
      const near = host.lan && host.ip && host.ip === from;
      return reply(near ? { host: host.addr, lan: host.lan } : { host: host.addr });
    }

    if (path === "/stop") {
      if (!isCode(b.code) || !isSecret(b.secret)) return reply({ error: "fields" }, 400);
      await db.batch([
        db.prepare("DELETE FROM joins WHERE code IN (SELECT code FROM hosts2 WHERE code = ? AND secret = ?)").bind(b.code, b.secret),
        db.prepare("DELETE FROM hosts2 WHERE code = ? AND secret = ?").bind(b.code, b.secret),
      ]);
      return reply({ ok: true });
    }
    return reply({ error: "path" }, 404);
  },
};
