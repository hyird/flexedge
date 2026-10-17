import assert from "node:assert/strict";

// End-to-end SSE check, run explicitly against an isolated backend.

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
            // A field-less frame is the server keep-alive; it is not an event.
            else if (Object.keys(event).length === 0) this.push({ keepAlive: true });
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
        // Readiness frames do not need JSON decoding.
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

async function verifyCertificatePagination() {
  const marker = `sse-pages-${Date.now()}`;
  const streams = [];
  try {
    for (const provider of ["letsencrypt", "zerossl"]) {
      const created = await request("/api/providers/certificate", {
        method: "POST",
        body: JSON.stringify({
          provider,
          credential_mode: "email",
          account_email: `${marker}-${provider}@example.com`,
        }),
      });
      assert.equal(created.response.status, 200);
    }
    const first = await request("/api/providers/certificate?page=1&page_size=1");
    const second = await request("/api/providers/certificate?page=2&page_size=1");
    for (const [index, result] of [first, second].entries()) {
      assert.equal(result.response.status, 200);
      assert.equal(result.body.data.list.length, 1);
      assert.equal(result.body.data.total, 2);
      assert.equal(result.body.data.page, index + 1);
      assert.equal(result.body.data.page_size, 1);
      assert.equal(result.body.data.total_pages, 2);
    }
    assert.notEqual(first.body.data.list[0].id, second.body.data.list[0].id);
    const options = await request("/api/providers/certificate/options?page=99&page_size=1");
    assert.equal(options.response.status, 200);
    assert.equal(options.body.data.length, 2, "options must ignore list pagination");
    for (const keyword of ["Let's Encrypt", "LET'S ENCRYPT", "ZeroSSL", `${marker}-letsencrypt`]) {
      const filtered = await request(`/api/providers/certificate?page_size=1&keyword=${encodeURIComponent(keyword)}`);
      assert.equal(filtered.body.data.list.length, 1);
      assert.equal(filtered.body.data.total, 1);
      const picker = await request(`/api/providers/certificate/options?page_size=1&keyword=${encodeURIComponent(keyword)}`);
      assert.deepEqual(picker.body.data, filtered.body.data.list);
    }
    const target = first.body.data.list[0];
    const label = target.provider === "letsencrypt" ? "Let's Encrypt" : "ZeroSSL";
    const paths = [
      "/api/providers/certificate/stream?page=1&page_size=1",
      "/api/providers/certificate/stream?page=2&page_size=1",
      `/api/providers/certificate/stream?page=1&page_size=1&keyword=${encodeURIComponent(label)}`,
      "/api/providers/certificate/stream?page=1&page_size=1&keyword=missing-provider",
    ];
    for (const path of paths) streams.push(await open(path));
    const snapshots = await Promise.all(streams.map((stream) => stream.snapshot()));
    for (const [index, path] of paths.entries()) {
      const get = await request(path.replace("/stream", ""));
      assert.deepEqual(listData(snapshots[index]), get.body.data, "GET/SSE page contract differs");
    }
    assert.equal(listData(snapshots[0]).list[0].id, target.id);
    assert.equal(listData(snapshots[1]).list[0].id, second.body.data.list[0].id);
    assert.equal(listData(snapshots[2]).list[0].id, target.id);
    assert.equal(listData(snapshots[3]).total_pages, 0);
    const invalidPaths = [
      "/api/providers/certificate?page=0",
      "/api/providers/certificate/stream?page_size=1001",
      `/api/providers/certificate/stream?keyword=${"x".repeat(201)}`,
    ];
    for (const path of invalidPaths) {
      const invalid = await request(path, { headers: { accept: "text/event-stream" } });
      assert.equal(invalid.response.status, 400, "GET/SSE must share query validation");
    }
    const removed = await request(`/api/providers/certificate/${target.id}`, {
      method: "DELETE",
      headers: { "if-match": `"${target.revision}"` },
    });
    assert.equal(removed.response.status, 200);
    const changed = await Promise.all(streams.slice(0, 3).map((stream) => stream.snapshot()));
    assert.equal(listData(changed[0]).list[0].id, second.body.data.list[0].id);
    assert.equal(listData(changed[0]).total, 1);
    assert.deepEqual(listData(changed[1]).list, []);
    assert.equal(listData(changed[1]).total, 1);
    assert.deepEqual(listData(changed[2]).list, []);
    assert.equal(listData(changed[2]).total, 0);
  } finally {
    for (const stream of streams) stream.close();
    const remaining = await request("/api/providers/certificate/options");
    assert.equal(remaining.response.status, 200);
    for (const item of remaining.body.data) {
      if (!item.account_email?.startsWith(marker)) continue;
      const removed = await request(`/api/providers/certificate/${item.id}`, {
        method: "DELETE",
        headers: { "if-match": `"${item.revision}"` },
      });
      assert.equal(removed.response.status, 200);
    }
  }
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
    const detail = await open(`/api/providers/dns/${providerId}/stream`);
    const certificateList = await open("/api/providers/certificate/stream?page=1&page_size=10");
    const all = [...listClients, detail, certificateList];
    const initial = await Promise.all(all.map((client) => client.snapshot()));
    original = providerData(initial[8]);
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
    const options = await request("/api/providers/dns/options");
    assert.equal(options.response.status, 200);
    assert.ok(Array.isArray(options.body.data));
    assert.ok(options.body.data.some((item) => item.id === providerId));
    assert.deepEqual(listData(initial[9]).list, []);
    assert.equal(listData(initial[9]).total, 0);
    assert.equal(listData(initial[9]).page_size, 10);
    await verifyCertificatePagination();

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
      detail.snapshot(),
    ]);
    for (const event of updated) {
      assert.equal(event.json.code, 0);
    }
    assert.equal(listData(updated[0]).list[0].name, newName);
    assert.equal(listData(updated[4]).list.length, 0);
    assert.equal(listData(updated[5]).list.length, 0);
    const updatedOptions = await request("/api/providers/dns/options");
    assert.equal(updatedOptions.body.data.find((item) => item.id === providerId).name, newName);
    assert.equal(providerData(updated[6]).name, newName);
    assert.equal(providerData(updated[6]).revision, revision + 1);

    const heartbeatBaseline = listClients[0].snapshotCount;
    await listClients[0].next((event) => event.keepAlive === true, 17000);
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
    console.log("Live resource HTTP integration passed: provider pagination/search/options, keyed projections, fanout updates, cancellation, heartbeat, logout, and re-login.");
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
