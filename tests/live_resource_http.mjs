import assert from "node:assert/strict";

// Run explicitly against an isolated backend; this is outside unit-test discovery.

const base = process.env.SSE_CACHE_QA_URL ?? "";
const password = process.env.SSE_CACHE_QA_PASSWORD;
const fixtureName = process.env.SSE_CACHE_QA_PROVIDER_NAME ?? "SSE isolated fixture";
const providerId = "10000000-0000-4000-8000-000000000001";

if (!base || !password) {
  throw new Error("SSE_CACHE_QA_URL and SSE_CACHE_QA_PASSWORD are required");
}
const parsedBase = new URL(base);
if (parsedBase.protocol !== "http:" || !["127.0.0.1", "localhost", "[::1]"].includes(parsedBase.hostname)) {
  throw new Error(`Refusing non-loopback QA server: ${parsedBase.hostname}`);
}

let cookie = "";
const clients = new Set();

async function request(path, options = {}) {
  const headers = new Headers(options.headers);
  headers.set("content-type", "application/json");
  if (cookie) headers.set("cookie", cookie);
  const response = await fetch(new URL(path, base), { ...options, headers });
  const setCookie = response.headers.get("set-cookie");
  if (setCookie) cookie = setCookie.split(";")[0];
  const text = await response.text();
  let body = null;
  if (text) {
    try {
      body = JSON.parse(text);
    } catch {
      body = text;
    }
  }
  return { response, body };
}

async function login() {
  const result = await request("/api/auth/login", {
    method: "POST",
    body: JSON.stringify({ username: "admin", password }),
  });
  assert.equal(result.response.status, 200);
}

class SseClient {
  constructor(path) {
    this.path = path;
    this.controller = new AbortController();
    this.events = [];
    this.waiters = [];
    this.snapshotCount = 0;
    clients.add(this);
  }

  async connect() {
    const response = await fetch(new URL(this.path, base), {
      headers: { accept: "text/event-stream", cookie },
      signal: this.controller.signal,
    });
    assert.equal(response.status, 200, `${this.path}: ${response.status}`);
    this.reader = response.body.getReader();
    this.pumpPromise = this.pump();
    return this;
  }

  async pump() {
    const decoder = new TextDecoder();
    let buffer = "";
    let event = {};
    try {
      while (true) {
        const { done, value } = await this.reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split(/\r?\n/);
        buffer = lines.pop() ?? "";
        for (const line of lines) {
          if (line === "") {
            if (event.event) this.push({ ...event });
            event = {};
          } else if (line.startsWith("event: ")) {
            event.event = line.slice(7);
          } else if (line.startsWith("data: ")) {
            event.data = (event.data ?? "") + line.slice(6);
          } else if (line.startsWith("id: ")) {
            event.id = line.slice(4);
          }
        }
      }
    } catch (error) {
      if (!this.controller.signal.aborted) this.push({ event: "error", error });
    }
  }

  push(event) {
    if (event.event === "snapshot") {
      event.json = JSON.parse(event.data);
      this.snapshotCount += 1;
    } else if (event.data) {
      try {
        event.json = JSON.parse(event.data);
      } catch {
        // Heartbeats and readiness frames do not need JSON decoding.
      }
    }
    let delivered = false;
    for (const waiter of [...this.waiters]) {
      if (waiter.predicate(event)) {
        this.waiters.splice(this.waiters.indexOf(waiter), 1);
        clearTimeout(waiter.timer);
        waiter.resolve(event);
        delivered = true;
      }
    }
    if (!delivered) this.events.push(event);
  }

  next(predicate, timeout = 10000) {
    const queued = this.events.find(predicate);
    if (queued) {
      this.events.splice(this.events.indexOf(queued), 1);
      return Promise.resolve(queued);
    }
    return new Promise((resolve, reject) => {
      const waiter = {
        predicate,
        resolve,
        timer: setTimeout(() => {
          this.waiters.splice(this.waiters.indexOf(waiter), 1);
          reject(new Error(`Timed out waiting for SSE event on ${this.path}`));
        }, timeout),
      };
      this.waiters.push(waiter);
    });
  }

  snapshot(timeout = 10000) {
    return this.next((event) => event.event === "snapshot", timeout);
  }

  close() {
    this.controller.abort();
    clients.delete(this);
  }
}

async function open(path) {
  return new SseClient(path).connect();
}

function listData(event) {
  assert.equal(event.event, "snapshot");
  assert.equal(event.json.code, 0);
  assert.ok(event.json.data && Array.isArray(event.json.data.list));
  return event.json.data;
}

function providerData(event) {
  assert.equal(event.event, "snapshot");
  assert.equal(event.json.code, 0);
  assert.ok(event.json.data && typeof event.json.data.id === "string");
  return event.json.data;
}

async function main() {
  await login();
  const oldName = fixtureName;
  let original;
  let restored = false;
  try {
    const listPaths = [
      `/api/providers/dns/stream?page=1&page_size=10`,
      `/api/providers/dns/stream?page_size=10&page=1`,
      `/api/providers/dns/stream?page=01&page_size=10`,
      `/api/providers/dns/stream?page_size=10&page=01`,
      `/api/providers/dns/stream?page=1&page_size=10&keyword=${encodeURIComponent(oldName)}`,
      `/api/providers/dns/stream?keyword=${encodeURIComponent(oldName)}&page_size=10&page=1`,
      `/api/providers/dns/stream?page=99&page_size=10`,
      `/api/providers/dns/stream?page_size=10&page=99`,
    ];
    const listClients = await Promise.all(listPaths.map(open));
    const options = await open("/api/providers/dns/options/stream");
    const detail = await open(`/api/providers/dns/${providerId}/stream`);
    const certificateList = await open("/api/providers/certificate/stream");
    const all = [...listClients, options, detail, certificateList];
    const initial = await Promise.all(all.map((client) => client.snapshot()));
    original = providerData(initial[9]);
    assert.equal(original.id, providerId);
    assert.equal(original.name, oldName);
    assert.equal(listData(initial[0]).page, 1);
    assert.equal(listData(initial[0]).page_size, 10);
    assert.deepEqual(listData(initial[0]).list, listData(initial[1]).list);
    assert.deepEqual(listData(initial[0]).list, listData(initial[2]).list);
    assert.deepEqual(listData(initial[0]).list, listData(initial[3]).list);
    assert.equal(listData(initial[4]).list.length, 1);
    assert.equal(listData(initial[6]).list.length, 0);
    assert.equal(listData(initial[7]).list.length, 0);
    assert.ok(Array.isArray(initial[8].json.data));
    assert.ok(initial[8].json.data.some((item) => item.id === providerId));
    assert.deepEqual(initial[10].json.data, []);

    const revision = original.revision;
    const newName = `SSE live resource ${Date.now()}`;
    const update = await request(`/api/providers/dns/${providerId}`, {
      method: "PUT",
      headers: { "if-match": `"${revision}"` },
      body: JSON.stringify({ name: newName }),
    });
    assert.equal(update.response.status, 200);

    listClients[6].close();
    const joined = await open("/api/providers/dns/stream?page=99&page_size=10");
    const joinedInitial = await joined.snapshot();
    assert.equal(listData(joinedInitial).list.length, 0);

    const updated = await Promise.all([
      ...listClients.slice(0, 6).map((client) => client.snapshot()),
      options.snapshot(),
      detail.snapshot(),
    ]);
    for (const event of updated) {
      assert.equal(event.json.code, 0);
    }
    assert.equal(listData(updated[0]).list[0].name, newName);
    assert.equal(listData(updated[4]).list.length, 0);
    assert.equal(listData(updated[5]).list.length, 0);
    assert.equal(updated[6].json.data[0].name, newName);
    assert.equal(providerData(updated[7]).name, newName);
    assert.equal(providerData(updated[7]).revision, revision + 1);

    const heartbeatBaseline = listClients[0].snapshotCount;
    await listClients[0].next((event) => event.event === "heartbeat", 17000);
    assert.equal(listClients[0].snapshotCount, heartbeatBaseline);

    const logout = await request("/api/auth/logout", { method: "POST" });
    assert.equal(logout.response.status, 200);
    await Promise.all([...all, joined].filter((client) => !client.controller.signal.aborted).map((client) =>
      client.next((event) => event.event === "session-expired", 18000)));
    const unauthorized = await fetch(new URL("/api/providers/dns/stream?page=1&page_size=10", base), {
      headers: { accept: "text/event-stream", cookie },
    });
    assert.equal(unauthorized.status, 401);

    await login();
    const fresh = await open(`/api/providers/dns/${providerId}/stream`);
    const freshSnapshot = await fresh.snapshot();
    assert.equal(providerData(freshSnapshot).name, newName);
    fresh.close();

    const restore = await request(`/api/providers/dns/${providerId}`, {
      method: "PUT",
      headers: { "if-match": `"${revision + 1}"` },
      body: JSON.stringify({ name: oldName }),
    });
    assert.equal(restore.response.status, 200);
    restored = true;
    console.log("Live resource HTTP integration passed: keyed projections, fanout updates, cancellation, heartbeat, logout, and re-login.");
  } finally {
    if (!restored && original) {
      try {
        await request(`/api/providers/dns/${providerId}`, {
          method: "PUT",
          headers: { "if-match": `"${original.revision + 1}"` },
          body: JSON.stringify({ name: oldName }),
        });
      } catch {
        // Preserve the original assertion failure; cleanup is best effort.
      }
    }
    for (const client of [...clients]) client.close();
  }
}

await main();
