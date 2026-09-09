import { hashKey, type QueryClient, type QueryKey } from '@tanstack/react-query'
import { getData } from './api'
import { ApiProtocolError, parseApiResponse, requireResponseData } from './api-response'
import { reconnectingEventSource } from './event-stream'

type Source = ReturnType<typeof reconnectingEventSource>
type Parse = (value: unknown) => unknown
type Patch = (current: unknown, value: unknown) => unknown
type Waiter = { resolve: (value: unknown) => void; reject: (error: unknown) => void; dispose: () => void }
type Binding = {
  key: QueryKey
  hash: string
  url: string
  parse: Parse
  patch?: Patch
  waiters: Set<Waiter>
}
type Entry = {
  url: string
  bindings: Map<string, Binding>
  source?: Source
  generation: number
  hasSnapshot: boolean
  latest?: unknown
}

/** One stream per URL, owned by active Query observers. Query functions await
 * the first frame; subsequent frames write the same cache without refetching.
 * Definitions survive an unmount so a cached query can reopen on remount.
 */
export class LiveQueryManager {
  private entries = new Map<string, Entry>()
  private definitions = new Map<string, Binding>()
  private epoch = 0
  private recovering?: Promise<void>
  private unsubscribe: () => void

  constructor(
    private client: QueryClient,
    private createSource: (url: string) => Source = reconnectingEventSource,
    private recoverSession: () => Promise<unknown> = () => getData('/auth/me'),
    private onSessionError: (error: unknown) => void = () => undefined
  ) {
    this.unsubscribe = client.getQueryCache().subscribe((event) => {
      const binding = this.definitions.get(event.query.queryHash)
      if (!binding) return
      if (event.type === 'removed') {
        this.detach(binding)
        this.definitions.delete(binding.hash)
        return
      }
      if (event.type === 'observerRemoved' || event.type === 'observerOptionsUpdated') {
        // StrictMode may detach and reattach in one turn; avoid extra streams.
        queueMicrotask(() => this.releaseInactive(binding))
      }
      if (event.type === 'observerAdded' || event.type === 'observerOptionsUpdated') {
        if (event.query.isActive() && !this.entries.get(binding.url)?.bindings.has(binding.hash)) {
          void event.query.fetch().catch(() => undefined)
        }
      }
    })
  }

  read<T>(url: string, key: QueryKey, signal?: AbortSignal,
          parse: (value: unknown) => T = (value) => value as T,
          patch?: (current: T | undefined, value: unknown) => T | undefined): Promise<T> {
    if (signal?.aborted) return Promise.reject(signal.reason)
    const hash = hashKey(key)
    const previous = this.definitions.get(hash)
    if (previous && previous.url !== url) this.detach(previous)
    const binding: Binding = previous?.url === url ? previous : {
      key, hash, url, parse, patch: patch as Patch | undefined, waiters: new Set(),
    }
    binding.parse = parse
    binding.patch = patch as Patch | undefined
    this.definitions.set(hash, binding)
    let entry = this.entries.get(url)
    const alreadyAttached = entry?.bindings.has(hash) ?? false
    if (!entry) {
      entry = { url, bindings: new Map(), generation: 0, hasSnapshot: false }
      this.entries.set(url, entry)
    }
    entry.bindings.set(hash, binding)
    const pending = new Promise<T>((resolve, reject) => {
      const waiter: Waiter = {
        resolve: (value) => resolve(value as T), reject,
        dispose: () => signal?.removeEventListener('abort', abort),
      }
      const abort = () => {
        binding.waiters.delete(waiter)
        waiter.dispose()
        reject(signal?.reason ?? new DOMException('读取已取消', 'AbortError'))
        queueMicrotask(() => this.releaseInactive(binding))
      }
      binding.waiters.add(waiter)
      signal?.addEventListener('abort', abort, { once: true })
    })
    if (!entry.source || (alreadyAttached && entry.hasSnapshot)) this.connect(entry)
    else if (entry.hasSnapshot) this.apply(binding, entry.latest)
    return pending
  }

  private active(binding: Binding) {
    return this.client.getQueryCache().find({ queryKey: binding.key, exact: true })?.isActive() ?? false
  }

  private releaseInactive(binding: Binding) {
    if (this.definitions.get(binding.hash) !== binding || this.active(binding)) return
    this.detach(binding)
  }

  private detach(binding: Binding) {
    this.settle(binding, undefined, new DOMException('订阅已结束', 'AbortError'))
    const entry = this.entries.get(binding.url)
    entry?.bindings.delete(binding.hash)
    if (entry && entry.bindings.size === 0) {
      this.retire(entry)
      this.entries.delete(entry.url)
    }
  }

  private settle(binding: Binding, value?: unknown, error?: unknown) {
    for (const waiter of binding.waiters) {
      waiter.dispose()
      if (error !== undefined) waiter.reject(error)
      else waiter.resolve(value)
    }
    binding.waiters.clear()
  }

  private fail(binding: Binding, error: unknown) {
    const query = this.client.getQueryCache().find({ queryKey: binding.key, exact: true })
    query?.setState({ error: error instanceof Error ? error : new ApiProtocolError(),
      status: 'error', fetchStatus: 'idle', errorUpdatedAt: Date.now() })
    this.settle(binding, undefined, error)
  }

  private apply(binding: Binding, value: unknown) {
    try {
      const parsed = binding.parse(value)
      this.client.setQueryData(binding.key, parsed)
      this.settle(binding, parsed)
    } catch (error) {
      this.fail(binding, error)
    }
    queueMicrotask(() => this.releaseInactive(binding))
  }

  private retire(entry: Entry) {
    entry.generation++
    entry.source?.close()
    entry.source = undefined
  }

  private connect(entry: Entry) {
    this.retire(entry)
    entry.hasSnapshot = false
    const generation = entry.generation
    const source = this.createSource(entry.url)
    entry.source = source
    const listen = (name: string, action: (event: Event) => void) => {
      source.addEventListener(name, (event) => {
        if (entry.generation !== generation || this.entries.get(entry.url) !== entry) return
        action(event)
      })
    }
    listen('snapshot', (event) => {
      try {
        const data = requireResponseData(parseApiResponse(this.payload(event)))
        entry.latest = data
        entry.hasSnapshot = true
        for (const binding of entry.bindings.values()) this.apply(binding, data)
      } catch (error) {
        for (const binding of entry.bindings.values()) this.fail(binding, error)
      }
    })
    listen('node-runtime', (event) => {
      if (!entry.hasSnapshot) return
      try {
        const patch = [...entry.bindings.values()].find((binding) => binding.patch)?.patch
        if (!patch) return
        const next = patch(entry.latest, this.payload(event))
        if (next === undefined || next === entry.latest) return
        entry.latest = next
        for (const binding of entry.bindings.values()) this.apply(binding, next)
      } catch (error) {
        for (const binding of entry.bindings.values()) this.fail(binding, error)
      }
    })
    listen('origin-runtime', (event) => {
      if (!entry.hasSnapshot) return
      try {
        const patch = [...entry.bindings.values()].find((binding) => binding.patch)?.patch
        if (!patch) return
        const next = patch(entry.latest, this.payload(event))
        if (next === undefined || next === entry.latest) return
        entry.latest = next
        for (const binding of entry.bindings.values()) this.apply(binding, next)
      } catch (error) {
        for (const binding of entry.bindings.values()) this.fail(binding, error)
      }
    })
    listen('resource-error', (event) => {
      let failure: unknown = new ApiProtocolError()
      try { parseApiResponse(this.payload(event)) } catch (error) { failure = error }
      for (const binding of entry.bindings.values()) this.fail(binding, failure)
    })
    listen('error', () => {
      for (const binding of entry.bindings.values())
        this.fail(binding, new Error('实时连接已中断，正在重新连接'))
      // EventSource does not expose a rejected handshake's HTTP status.
      // Probe auth only on transport failure, shared across subscriptions.
      this.checkSession()
    })
    listen('session-expired', () => {
      this.retire(entry)
      this.checkSession(true)
    })
  }

  private payload(event: Event): unknown {
    if (!(event instanceof MessageEvent) || typeof event.data !== 'string') throw new ApiProtocolError()
    try { return JSON.parse(event.data) } catch { throw new ApiProtocolError() }
  }

  private checkSession(reconnect = false) {
    if (this.recovering) return
    const epoch = this.epoch
    const recovery = this.recoverSession().then(() => {
      if (this.epoch !== epoch) return
      for (const entry of this.entries.values())
        if (reconnect || !entry.source) this.connect(entry)
    }).catch((error: unknown) => {
      if (this.epoch !== epoch) return
      this.onSessionError(error)
      if (this.epoch === epoch) {
        for (const entry of this.entries.values())
          if (!entry.source) this.connect(entry)
      }
    }).finally(() => {
      if (this.recovering === recovery) this.recovering = undefined
    })
    this.recovering = recovery
  }

  close() {
    this.epoch++
    this.recovering = undefined
    for (const entry of this.entries.values()) {
      this.retire(entry)
      for (const binding of entry.bindings.values())
        this.settle(binding, undefined, new DOMException('会话已结束', 'AbortError'))
    }
    this.entries.clear()
    this.definitions.clear()
  }

  dispose() { this.close(); this.unsubscribe() }
}

let manager: LiveQueryManager | undefined

export function setLiveQueryQueryClient(client: QueryClient, onSessionError?: (error: unknown) => void,
  createSource?: (url: string) => Source) {
  manager?.dispose()
  manager = new LiveQueryManager(client, createSource, undefined, onSessionError)
}

export function readLiveQuery<T>(url: string, key: QueryKey, signal?: AbortSignal,
  parse?: (value: unknown) => T, patch?: (current: T | undefined, value: unknown) => T | undefined) {
  if (!manager) throw new Error('实时查询客户端尚未初始化')
  return manager.read(url, key, signal, parse, patch)
}

export function closeLiveQueries() { manager?.close() }
