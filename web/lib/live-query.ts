import {
  hashKey,
  notifyManager,
  type Query,
  type QueryClient,
  type QueryKey,
  type QueryObserver,
} from '@tanstack/react-query'
import {
  ApiProtocolError,
  parseApiResponse,
  requireResponseData,
} from './api-response'
import {
  openEventStream,
  streamExhaustedEvent,
  streamSessionExpiredEvent,
  type EventStream,
} from './event-stream'
import { type LoadingEpoch, MinimumLoading } from './minimum-loading'

type Parse = (value: unknown) => unknown
type Patch = (current: unknown, value: unknown, event: string) => unknown
type Merge = (current: unknown, frame: unknown) => unknown
type Waiter = {
  resolve: (value: unknown) => void
  reject: (error: unknown) => void
  dispose: () => void
}
type Binding = {
  key: QueryKey
  hash: string
  url: string
  /** Event name whose frames feed this query (`snapshot` for single streams). */
  event: string
  parse: Parse
  patch?: Patch
  /** Append binding: frames fold into the current data (log tails). */
  merge?: Merge
  waiters: Set<Waiter>
}
type Entry = {
  url: string
  bindings: Map<string, Binding>
  /** Activation order: the newest entry owns the page's connection. */
  order: number
  source?: EventStream
  /** Latest envelope data per frame event on the current connection. */
  frames: Map<string, unknown>
  /** Frame events with a listener on the current connection. */
  listening: Set<string>
}

/** Recovers the session after the server ended it on a stream; resolves
 * true when it is valid again and the stream may reconnect. */
export type RecoverSession = () => Promise<boolean>

/** Plain-JSON patch events; each applies to the frames of bindings with
 * `patch` (`node-runtime`/`origin-runtime` runtime patches, and `attempt`:
 * a new execution record on a task detail stream). */
const patchEvents = ['node-runtime', 'origin-runtime', 'attempt']

/** The page's live views: queries bound to the named events of SSE streams,
 * owned by their active Query observers. Query functions await the first
 * frame; later frames write the same cache without refetching. Definitions
 * survive an unmount so a cached query can reattach on remount.
 *
 * One registry, one connection: of all streams with active views, only the
 * most recently activated one is connected. A view that opens over another
 * (a sheet over a list, a tab's tail under a live header) switches the
 * page's connection to its stream; when it closes, the connection switches
 * back and the underlying view receives a recovery snapshot. Views of one
 * URL share the connection; the last view to leave releases it. A newly
 * opened view takes the connection at once, closing the previous one first;
 * departures are reconciled once per microtask, so a StrictMode remount
 * keeps its connection. One-off reads go over GET, never through here.
 *
 * First loads that a view shows as loading (no cached data, no previous
 * data kept as placeholder) reveal their first frame or error no earlier
 * than `minimumLoadingMs` after that view started showing its loading state,
 * or with an `epoch` after the route change or tab switch that started the
 * loading sequence (`MinimumLoading`, `LoadingEpoch`). Cached data, refetches
 * of loaded data and live frames are never delayed.
 */
export class LiveQueryManager {
  private entries = new Map<string, Entry>()
  private definitions = new Map<string, Binding>()
  private connected?: Entry
  private order = 0
  private epoch = 0
  private recovering?: Promise<void>
  private reconcileQueued = false
  private loading: MinimumLoading<Binding>
  /** Since when each observer shows a loading state for missing data: from
   * mounting or from switching to an uncached key, kept across further
   * uncached keys (e.g. a list whose rows decide the next query). */
  private loadingSince = new WeakMap<QueryObserver, number>()
  private unsubscribe: () => void

  constructor(
    private client: QueryClient,
    private createSource: (url: string) => EventStream = (url) =>
      openEventStream(url),
    private recoverSession: RecoverSession = () => Promise.resolve(true),
    minimumLoadingMs = 0,
    epoch?: LoadingEpoch
  ) {
    this.loading = new MinimumLoading(
      minimumLoadingMs,
      notifyManager.batch,
      epoch
    )
    this.unsubscribe = client.getQueryCache().subscribe((event) => {
      if (event.type === 'observerAdded')
        this.trackLoading(event.query, event.observer)
      const binding = this.definitions.get(event.query.queryHash)
      if (!binding) return
      if (event.type === 'removed') {
        this.detach(binding)
        this.definitions.delete(binding.hash)
        this.queueReconcile()
        return
      }
      if (
        event.type === 'observerRemoved' ||
        event.type === 'observerOptionsUpdated'
      )
        this.queueReconcile()
      if (
        (event.type === 'observerAdded' ||
          event.type === 'observerOptionsUpdated') &&
        event.query.isActive() &&
        !this.entries.get(binding.url)?.bindings.has(binding.hash)
      )
        void event.query.fetch().catch(() => undefined)
    })
  }

  private trackLoading(query: Query, observer: QueryObserver) {
    // The observer's result still belongs to its previous key.
    const shown = observer.getCurrentResult().data
    if (
      query.state.data !== undefined ||
      (shown !== undefined && observer.options.placeholderData !== undefined)
    )
      this.loadingSince.delete(observer)
    else if (shown !== undefined || !this.loadingSince.has(observer))
      this.loadingSince.set(observer, Date.now())
  }

  /** Binds one named event of a stream to a query. `patch` receives the
   * plain-JSON patch events with their name and returns the new frame, or
   * `undefined` to leave it unchanged; `merge` folds each frame into the
   * current data instead of replacing it. */
  bind<T>(
    url: string,
    event: string,
    key: QueryKey,
    signal: AbortSignal | undefined,
    options: {
      parse?: (value: unknown) => T
      patch?: (
        current: T | undefined,
        value: unknown,
        event: string
      ) => T | undefined
      merge?: (current: T | undefined, frame: T) => T
    } = {}
  ): Promise<T> {
    if (signal?.aborted) return Promise.reject(signal.reason)
    const hash = hashKey(key)
    const previous = this.definitions.get(hash)
    const reusable = previous?.url === url && previous.event === event
    if (previous && !reusable) this.detach(previous)
    const binding: Binding = reusable
      ? previous
      : {
          key,
          hash,
          url,
          event,
          parse: (value) => value,
          waiters: new Set(),
        }
    binding.parse = (options.parse as Parse | undefined) ?? ((value) => value)
    binding.patch = options.patch as Patch | undefined
    binding.merge = options.merge as Merge | undefined
    this.definitions.set(hash, binding)
    let entry = this.entries.get(url)
    if (!entry) {
      entry = {
        url,
        bindings: new Map(),
        order: 0,
        frames: new Map(),
        listening: new Set(),
      }
      this.entries.set(url, entry)
    }
    const alreadyAttached = entry.bindings.has(hash)
    // A newly attached view takes the page's connection; a manual refetch of
    // an attached one does not.
    if (!alreadyAttached) entry.order = ++this.order
    entry.bindings.set(hash, binding)
    const pending = new Promise<T>((resolve, reject) => {
      const waiter: Waiter = {
        resolve: (value) => resolve(value as T),
        reject,
        dispose: () => signal?.removeEventListener('abort', abort),
      }
      const abort = () => {
        binding.waiters.delete(waiter)
        waiter.dispose()
        reject(signal?.reason ?? new DOMException('读取已取消', 'AbortError'))
        this.queueReconcile()
      }
      binding.waiters.add(waiter)
      signal?.addEventListener('abort', abort, { once: true })
    })
    const hasFrame = entry.frames.has(event)
    // A frame of the shared connection already in memory counts as cached.
    const since = hasFrame ? undefined : this.shownLoadingSince(key)
    if (since !== undefined) this.loading.start(binding, since)
    // A newly attached view is the newest: it takes the connection at once
    // (the previous one closes first). Departed views leave in `reconcile`.
    if (entry !== this.connected) {
      if (alreadyAttached) this.queueReconcile()
      else this.switchTo(entry)
    }
    // A manual refetch of an attached query reconnects to await a fresh
    // frame, as does any read of a stream that stopped (exhausted).
    else if (!entry.source || (alreadyAttached && hasFrame)) this.connect(entry)
    else if (hasFrame) this.apply(binding, entry.frames.get(event))
    else this.listenFrame(entry, event)
    return pending
  }

  /** Earliest time a view of the query started showing its loading state;
   * undefined when the query has data or every view keeps showing previous
   * data in its place (`keepPreviousData`). */
  private shownLoadingSince(key: QueryKey) {
    const query = this.client
      .getQueryCache()
      .find({ queryKey: key, exact: true })
    if (!query || query.state.data !== undefined) return undefined
    let since: number | undefined
    for (const observer of query.observers) {
      const shown = this.loadingSince.get(observer)
      if (shown !== undefined && (since === undefined || shown < since))
        since = shown
    }
    return since
  }

  private queueReconcile() {
    if (this.reconcileQueued) return
    this.reconcileQueued = true
    queueMicrotask(() => this.reconcile())
  }

  /** Releases views without an active observer and hands the page's
   * connection to the newest live stream. */
  private reconcile() {
    this.reconcileQueued = false
    for (const entry of [...this.entries.values()])
      for (const binding of [...entry.bindings.values()]) {
        const retained =
          this.client
            .getQueryCache()
            .find({ queryKey: binding.key, exact: true })
            ?.isActive() ?? false
        if (!retained) this.detach(binding)
      }
    let newest: Entry | undefined
    for (const entry of this.entries.values())
      if (!newest || entry.order > newest.order) newest = entry
    if (newest !== this.connected) this.switchTo(newest)
  }

  /** Makes `entry` the page's only connection, closing the previous one. */
  private switchTo(entry: Entry | undefined) {
    if (this.connected) this.retire(this.connected)
    this.connected = entry
    // A session recovery in progress connects it once the session is valid.
    if (entry && !this.recovering) this.connect(entry)
  }

  private detach(binding: Binding) {
    this.loading.cancel(binding)
    this.settle(
      binding,
      undefined,
      new DOMException('订阅已结束', 'AbortError')
    )
    const entry = this.entries.get(binding.url)
    if (entry?.bindings.get(binding.hash) !== binding) return
    entry.bindings.delete(binding.hash)
    if (entry.bindings.size) return
    this.retire(entry)
    this.entries.delete(entry.url)
    if (this.connected === entry) this.connected = undefined
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
    this.loading.reveal(binding, () => {
      const query = this.client
        .getQueryCache()
        .find({ queryKey: binding.key, exact: true })
      query?.setState({
        error: error instanceof Error ? error : new ApiProtocolError(),
        status: 'error',
        fetchStatus: 'idle',
        errorUpdatedAt: Date.now(),
      })
      this.settle(binding, undefined, error)
    })
  }

  private apply(binding: Binding, value: unknown) {
    this.loading.reveal(binding, () => {
      try {
        const parsed = binding.parse(value)
        const next = binding.merge
          ? binding.merge(this.client.getQueryData(binding.key), parsed)
          : parsed
        this.client.setQueryData(binding.key, next)
        this.settle(binding, next)
      } catch (error) {
        this.fail(binding, error)
      }
      this.queueReconcile()
    })
  }

  private retire(entry: Entry) {
    entry.source?.close()
    entry.source = undefined
    entry.frames.clear()
    entry.listening.clear()
  }

  /** Adds a listener that ignores events of retired connections. */
  private listen(entry: Entry, name: string, action: (event: Event) => void) {
    const source = entry.source
    source?.addEventListener(name, (event) => {
      if (entry.source === source) action(event)
    })
  }

  private listenFrame(entry: Entry, name: string) {
    if (!entry.source || entry.listening.has(name)) return
    entry.listening.add(name)
    this.listen(entry, name, (event) => {
      const bindings = [...entry.bindings.values()].filter(
        (binding) => binding.event === name
      )
      try {
        const data = requireResponseData(parseApiResponse(this.payload(event)))
        entry.frames.set(name, data)
        for (const binding of bindings) this.apply(binding, data)
      } catch (error) {
        for (const binding of bindings) this.fail(binding, error)
      }
    })
  }

  private connect(entry: Entry) {
    this.retire(entry)
    const source = this.createSource(entry.url)
    entry.source = source
    for (const event of new Set(
      [...entry.bindings.values()].map((binding) => binding.event)
    ))
      this.listenFrame(entry, event)
    for (const name of patchEvents)
      this.listen(entry, name, (event) => {
        try {
          const value = this.payload(event)
          const patched = new Set<string>()
          for (const binding of [...entry.bindings.values()]) {
            if (
              !binding.patch ||
              patched.has(binding.event) ||
              !entry.frames.has(binding.event)
            )
              continue
            patched.add(binding.event)
            const current = entry.frames.get(binding.event)
            const next = binding.patch(current, value, name)
            if (next === undefined || next === current) continue
            entry.frames.set(binding.event, next)
            for (const target of entry.bindings.values())
              if (target.event === binding.event) this.apply(target, next)
          }
        } catch (error) {
          for (const binding of entry.bindings.values())
            this.fail(binding, error)
        }
      })
    this.listen(entry, 'resource-error', (event) => {
      let failure: unknown = new ApiProtocolError()
      try {
        parseApiResponse(this.payload(event))
      } catch (error) {
        failure = error
      }
      for (const binding of entry.bindings.values()) this.fail(binding, failure)
    })
    // The stream reconnects by itself; views keep their data meanwhile.
    this.listen(entry, 'error', () => {
      for (const binding of entry.bindings.values())
        this.fail(binding, new Error('实时连接已中断，正在重新连接'))
    })
    // Refused handshakes exhausted the bounded retries (e.g. an endpoint this
    // server does not provide): stop, and let a manual refetch reconnect.
    this.listen(entry, streamExhaustedEvent, () => {
      this.retire(entry)
      for (const binding of entry.bindings.values())
        this.fail(binding, new Error('实时数据暂不可用，请稍后手动重试'))
    })
    this.listen(entry, streamSessionExpiredEvent, () => {
      this.retire(entry)
      this.recover()
    })
  }

  private payload(event: Event): unknown {
    if (!(event instanceof MessageEvent) || typeof event.data !== 'string')
      throw new ApiProtocolError()
    try {
      return JSON.parse(event.data)
    } catch {
      throw new ApiProtocolError()
    }
  }

  /** The only session path of live views: recover once, then reconnect the
   * page's stream; an ended session closes every view (`close`). */
  private recover() {
    if (this.recovering) return
    const epoch = this.epoch
    const recovery = this.recoverSession()
      .catch(() => false)
      .then((valid) => {
        if (this.recovering === recovery) this.recovering = undefined
        if (!valid || this.epoch !== epoch) return
        const entry = this.connected
        if (entry && !entry.source) this.connect(entry)
      })
    this.recovering = recovery
  }

  close() {
    this.epoch++
    this.recovering = undefined
    this.loading.clear()
    for (const entry of this.entries.values()) this.retire(entry)
    for (const binding of this.definitions.values())
      this.settle(
        binding,
        undefined,
        new DOMException('会话已结束', 'AbortError')
      )
    this.entries.clear()
    this.definitions.clear()
    this.connected = undefined
  }

  dispose() {
    this.close()
    this.unsubscribe()
  }
}

let manager: LiveQueryManager | undefined

function requireManager() {
  if (!manager) throw new Error('实时查询客户端尚未初始化')
  return manager
}

/** Installs the live-query manager. The console passes its session recovery
 * (`main.tsx`), `minimumLoadingMs` and its `loadingEpoch`
 * (`web/lib/minimum-loading.ts`); tests reading fake streams pass
 * `createSource` and receive first frames at once. */
export function setLiveQueryQueryClient(
  client: QueryClient,
  recoverSession?: RecoverSession,
  createSource?: (url: string) => EventStream,
  minimumLoadingMs = 0,
  epoch?: LoadingEpoch
) {
  manager?.dispose()
  manager = new LiveQueryManager(
    client,
    createSource,
    recoverSession,
    minimumLoadingMs,
    epoch
  )
}

/** Live view of a single-resource stream (`snapshot` frames). */
export function readLiveQuery<T>(
  url: string,
  key: QueryKey,
  signal?: AbortSignal,
  parse?: (value: unknown) => T,
  patch?: (
    current: T | undefined,
    value: unknown,
    event: string
  ) => T | undefined
) {
  return requireManager().bind(url, 'snapshot', key, signal, { parse, patch })
}

/** Reads one named event of a combined stream (e.g. `/api/overview/stream`
 * events `snapshot`, `traffic`, `nodes`) into its own query. All queries on
 * the same URL share the page's connection. */
export function readLiveEvent<T>(
  url: string,
  event: string,
  key: QueryKey,
  signal?: AbortSignal,
  parse?: (value: unknown) => T,
  patch?: (
    current: T | undefined,
    value: unknown,
    event: string
  ) => T | undefined
) {
  return requireManager().bind(url, event, key, signal, { parse, patch })
}

/** Append view of a tail stream (log tails): each `event` frame is parsed
 * and folded into the current data with `merge`. The stream resumes from
 * its last event ID after an interruption. */
export function readLiveAppend<T>(
  url: string,
  event: string,
  key: QueryKey,
  signal: AbortSignal | undefined,
  parse: (value: unknown) => T,
  merge: (current: T | undefined, frame: T) => T
) {
  return requireManager().bind(url, event, key, signal, { parse, merge })
}

export function closeLiveQueries() {
  manager?.close()
}
