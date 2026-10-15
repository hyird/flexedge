import {
  hashKey,
  notifyManager,
  queryOptions,
  type Query,
  type QueryClient,
  type QueryKey,
  type QueryObserver,
} from '@tanstack/react-query'
import { getData } from './api'
import {
  ApiProtocolError,
  parseApiResponse,
  requireResponseData,
} from './api-response'
import { reconnectingEventSource, streamExhaustedEvent } from './event-stream'
import { type LoadingEpoch, MinimumLoading } from './minimum-loading'

type Source = ReturnType<typeof reconnectingEventSource>
type Parse = (value: unknown) => unknown
type Patch = (current: unknown, value: unknown) => unknown
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
  snapshotOnly: boolean
  waiters: Set<Waiter>
}
type Entry = {
  url: string
  bindings: Map<string, Binding>
  source?: Source
  generation: number
  /** Latest envelope data per frame event on the current connection. */
  frames: Map<string, unknown>
  /** Frame events with a listener on the current connection. */
  listening: Set<string>
  /** A transport failure already probed the session since the last open. */
  sessionProbed?: boolean
}

/** Runtime patch events; each applies to the frames of bindings with `patch`. */
const patchEvents = ['node-runtime', 'origin-runtime']

/** One-shot reads close after the first frame. Configuration pickers use the
 * existing snapshot endpoints once; they have no realtime requirement. */
function isSnapshotOnly(url: string, key: QueryKey) {
  return (
    /\/(options|collection|available|history)\/stream(?:\?|$)/.test(url) ||
    key.includes('lines') ||
    key.includes('history')
  )
}

/** One stream per URL, owned by active Query observers. Query functions await
 * the first frame; subsequent frames write the same cache without refetching.
 * Definitions survive an unmount so a cached query can reopen on remount.
 *
 * First loads that a view shows as loading (no cached data, no previous
 * data kept as placeholder) reveal their first frame or error no earlier
 * than `minimumLoadingMs` after that view started showing its loading state,
 * or with an `epoch` after the route change or tab switch that started the
 * loading sequence (`MinimumLoading`, `LoadingEpoch`), so every consumer of
 * the query leaves its skeleton in the same render and a read that starts
 * after an earlier step is not held again. Cached data, refetches of loaded
 * data and live frames are never delayed.
 *
 * Back/forward cache: each stream closes itself on `pagehide` and reconnects
 * once on restore (`reconnectingEventSource`). One-shot reads still waiting
 * for their snapshot are cancelled on `pagehide` (their query reverts) and
 * read again on restore if still observed.
 */
export class LiveQueryManager {
  private entries = new Map<string, Entry>()
  private definitions = new Map<string, Binding>()
  private epoch = 0
  private recovering?: Promise<void>
  private unsubscribe: () => void
  /** Bindings whose observers left; released in a microtask (StrictMode may
   * reattach in the same turn) or right before a new stream opens. */
  private pendingRelease = new Set<Binding>()
  /** One-shot reads cancelled by `pagehide`, re-read when the page returns. */
  private cancelledSnapshots: Binding[] = []
  private loading: MinimumLoading<Binding>
  /** Since when each observer shows a loading state for missing data: from
   * mounting or from switching to an uncached key, kept across further
   * uncached keys (e.g. a list whose rows decide the next query). */
  private loadingSince = new WeakMap<QueryObserver, number>()
  private onPageHide = () => this.cancelSnapshots()
  private onPageShow = (event: Event) => {
    if ((event as PageTransitionEvent).persisted) this.rereadSnapshots()
  }

  constructor(
    private client: QueryClient,
    // Comment heartbeats are invisible to EventSource. Native transport errors
    // own reconnects; silence must not trigger repeated snapshot queries.
    private createSource: (url: string) => Source = (url) =>
      reconnectingEventSource(url, { idleTimeoutMs: null }),
    private recoverSession: () => Promise<unknown> = () => getData('/auth/me'),
    private onSessionError: (error: unknown) => void = () => undefined,
    private pageLifecycle: EventTarget | null = typeof window === 'undefined'
      ? null
      : window,
    minimumLoadingMs = 0,
    epoch?: LoadingEpoch
  ) {
    this.loading = new MinimumLoading(
      minimumLoadingMs,
      notifyManager.batch,
      epoch
    )
    pageLifecycle?.addEventListener('pagehide', this.onPageHide)
    pageLifecycle?.addEventListener('pageshow', this.onPageShow)
    this.unsubscribe = client.getQueryCache().subscribe((event) => {
      if (event.type === 'observerAdded')
        this.trackLoading(event.query, event.observer)
      const binding = this.definitions.get(event.query.queryHash)
      if (!binding) return
      if (event.type === 'removed') {
        this.detach(binding)
        this.definitions.delete(binding.hash)
        return
      }
      if (
        event.type === 'observerRemoved' ||
        event.type === 'observerOptionsUpdated'
      ) {
        // StrictMode may detach and reattach in one turn; avoid extra streams.
        this.pendingRelease.add(binding)
        queueMicrotask(() => this.releaseDeparted())
      }
      if (
        event.type === 'observerAdded' ||
        event.type === 'observerOptionsUpdated'
      ) {
        if (
          !binding.snapshotOnly &&
          event.query.isActive() &&
          !this.entries.get(binding.url)?.bindings.has(binding.hash)
        ) {
          void event.query.fetch().catch(() => undefined)
        }
      }
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

  read<T>(
    url: string,
    key: QueryKey,
    signal?: AbortSignal,
    parse: (value: unknown) => T = (value) => value as T,
    patch?: (current: T | undefined, value: unknown) => T | undefined
  ): Promise<T> {
    return this.bind(url, 'snapshot', key, signal, parse, patch, false)
  }

  /** Reads exactly one snapshot and releases the stream even while observed,
   * for one-off lookups such as command palette search on list endpoints. */
  readSnapshot<T>(
    url: string,
    key: QueryKey,
    signal?: AbortSignal,
    parse: (value: unknown) => T = (value) => value as T
  ): Promise<T> {
    return this.bind(url, 'snapshot', key, signal, parse, undefined, true)
  }

  /** Binds one named event of a combined stream to a query. Every binding of
   * the same URL shares one EventSource, which stays open while any of those
   * queries is observed. */
  readEvent<T>(
    url: string,
    event: string,
    key: QueryKey,
    signal?: AbortSignal,
    parse: (value: unknown) => T = (value) => value as T,
    patch?: (current: T | undefined, value: unknown) => T | undefined
  ): Promise<T> {
    return this.bind(url, event, key, signal, parse, patch, false)
  }

  private bind<T>(
    url: string,
    event: string,
    key: QueryKey,
    signal: AbortSignal | undefined,
    parse: (value: unknown) => T,
    patch:
      ((current: T | undefined, value: unknown) => T | undefined) | undefined,
    explicitSnapshot: boolean
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
          parse,
          patch: patch as Patch | undefined,
          waiters: new Set(),
          snapshotOnly: explicitSnapshot || isSnapshotOnly(url, key),
        }
    binding.parse = parse
    binding.patch = patch as Patch | undefined
    this.definitions.set(hash, binding)
    let entry = this.entries.get(url)
    const alreadyAttached = entry?.bindings.has(hash) ?? false
    if (!entry) {
      entry = {
        url,
        bindings: new Map(),
        generation: 0,
        frames: new Map(),
        listening: new Set(),
      }
      this.entries.set(url, entry)
    }
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
        queueMicrotask(() => this.releaseInactive(binding))
      }
      binding.waiters.add(waiter)
      signal?.addEventListener('abort', abort, { once: true })
    })
    const hasFrame = entry.frames.has(event)
    // A frame of the shared stream already in memory counts as cached.
    const since = hasFrame ? undefined : this.shownLoadingSince(key)
    if (since !== undefined) this.loading.start(binding, since)
    // A manual refetch of an attached query reconnects to await a fresh frame.
    if (!entry.source || (alreadyAttached && hasFrame)) this.connect(entry)
    else if (hasFrame) {
      this.apply(binding, entry.frames.get(event))
      if (binding.snapshotOnly) this.unbind(binding)
    } else this.listenFrame(entry, event)
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

  private active(binding: Binding) {
    return (
      this.client
        .getQueryCache()
        .find({ queryKey: binding.key, exact: true })
        ?.isActive() ?? false
    )
  }

  /** Releases departed bindings now. Runs before any new stream opens, so a
   * route or tab switch closes the old stream before connecting the new one. */
  releaseDeparted() {
    const bindings = [...this.pendingRelease]
    this.pendingRelease.clear()
    for (const binding of bindings) this.releaseInactive(binding)
  }

  private releaseInactive(binding: Binding) {
    if (this.definitions.get(binding.hash) !== binding || this.active(binding))
      return
    this.detach(binding)
  }

  private detach(binding: Binding) {
    this.loading.cancel(binding)
    this.settle(
      binding,
      undefined,
      new DOMException('订阅已结束', 'AbortError')
    )
    this.unbind(binding)
  }

  /** Removes the binding from its stream, closing the stream after the last
   * one. A one-shot read whose snapshot is still held keeps its waiters. */
  private unbind(binding: Binding) {
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
    if (binding.snapshotOnly) this.unbind(binding)
  }

  private apply(binding: Binding, value: unknown) {
    this.loading.reveal(binding, () => {
      try {
        const parsed = binding.parse(value)
        this.client.setQueryData(binding.key, parsed)
        this.settle(binding, parsed)
      } catch (error) {
        this.fail(binding, error)
      }
      queueMicrotask(() => this.releaseInactive(binding))
    })
  }

  private retire(entry: Entry) {
    entry.generation++
    entry.source?.close()
    entry.source = undefined
  }

  /** Adds a source listener that ignores frames from retired connections. */
  private listen(entry: Entry, name: string, action: (event: Event) => void) {
    const source = entry.source
    const generation = entry.generation
    source?.addEventListener(name, (event) => {
      if (
        entry.generation !== generation ||
        this.entries.get(entry.url) !== entry
      )
        return
      action(event)
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
        for (const binding of bindings)
          if (binding.snapshotOnly) this.unbind(binding)
      } catch (error) {
        for (const binding of bindings) this.fail(binding, error)
      }
    })
  }

  private connect(entry: Entry) {
    // Close streams whose observers already left in this commit first, so the
    // page never holds the old and the new stream at the same time. The flush
    // can retire this very entry (e.g. a session-recovery reconnect after its
    // last observer left); a retired entry must not open a new source.
    this.releaseDeparted()
    if (this.entries.get(entry.url) !== entry) return
    this.retire(entry)
    entry.frames.clear()
    entry.listening.clear()
    entry.source = this.createSource(entry.url)
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
            const next = binding.patch(current, value)
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
    this.listen(entry, 'error', () => {
      for (const binding of entry.bindings.values())
        this.fail(
          binding,
          new Error(
            binding.snapshotOnly
              ? '数据读取失败，请重试'
              : '实时连接已中断，正在重新连接'
          )
        )
      // EventSource does not expose a rejected handshake's HTTP status.
      // Probe auth once per outage, shared across subscriptions; repeated
      // failures of the same connection attempt series do not re-probe.
      if (entry.sessionProbed) return
      entry.sessionProbed = true
      this.checkSession()
    })
    this.listen(entry, 'open', () => {
      entry.sessionProbed = false
    })
    // Refused handshakes exhausted the bounded retries (e.g. an endpoint this
    // server does not provide): stop, and let a manual refetch reconnect.
    this.listen(entry, streamExhaustedEvent, () => {
      this.retire(entry)
      for (const binding of entry.bindings.values())
        this.fail(binding, new Error('实时数据暂不可用，请稍后手动重试'))
    })
    this.listen(entry, 'session-expired', () => {
      this.retire(entry)
      this.checkSession(true)
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

  private checkSession(reconnect = false) {
    if (this.recovering) return
    const epoch = this.epoch
    const recovery = this.recoverSession()
      .then(() => {
        if (this.epoch !== epoch) return
        for (const entry of this.entries.values())
          if (reconnect || !entry.source) this.connect(entry)
      })
      .catch((error: unknown) => {
        if (this.epoch !== epoch) return
        this.onSessionError(error)
        if (this.epoch === epoch) {
          for (const entry of this.entries.values())
            if (!entry.source) this.connect(entry)
        }
      })
      .finally(() => {
        if (this.recovering === recovery) this.recovering = undefined
      })
    this.recovering = recovery
  }

  close() {
    this.epoch++
    this.recovering = undefined
    this.loading.clear()
    for (const entry of this.entries.values()) this.retire(entry)
    // Definitions include one-shot reads whose held snapshot left the stream.
    for (const binding of this.definitions.values())
      this.settle(
        binding,
        undefined,
        new DOMException('会话已结束', 'AbortError')
      )
    this.entries.clear()
    this.definitions.clear()
    this.cancelledSnapshots = []
  }

  /** Cancels in-flight one-shot reads so no snapshot request outlives the
   * page; the query reverts to its previous state instead of failing. */
  private cancelSnapshots() {
    for (const entry of [...this.entries.values()])
      for (const binding of [...entry.bindings.values()]) {
        if (!binding.snapshotOnly) continue
        this.cancelledSnapshots.push(binding)
        void this.client
          .getQueryCache()
          .find({ queryKey: binding.key, exact: true })
          ?.cancel({ revert: true })
        this.detach(binding)
      }
  }

  private rereadSnapshots() {
    const bindings = this.cancelledSnapshots.splice(0)
    for (const binding of bindings) {
      if (this.definitions.get(binding.hash) !== binding) continue
      const query = this.client
        .getQueryCache()
        .find({ queryKey: binding.key, exact: true })
      if (query?.isActive()) void query.fetch().catch(() => undefined)
    }
  }

  dispose() {
    this.close()
    this.unsubscribe()
    this.pageLifecycle?.removeEventListener('pagehide', this.onPageHide)
    this.pageLifecycle?.removeEventListener('pageshow', this.onPageShow)
  }
}

let manager: LiveQueryManager | undefined

/** Installs the live-query manager. The console passes `minimumLoadingMs`
 * and its `loadingEpoch` (`web/lib/minimum-loading.ts`); tests reading fake
 * streams omit them and receive first frames at once. */
export function setLiveQueryQueryClient(
  client: QueryClient,
  onSessionError?: (error: unknown) => void,
  createSource?: (url: string) => Source,
  minimumLoadingMs = 0,
  epoch?: LoadingEpoch
) {
  manager?.dispose()
  manager = new LiveQueryManager(
    client,
    createSource,
    undefined,
    onSessionError,
    undefined,
    minimumLoadingMs,
    epoch
  )
}

export function readLiveQuery<T>(
  url: string,
  key: QueryKey,
  signal?: AbortSignal,
  parse?: (value: unknown) => T,
  patch?: (current: T | undefined, value: unknown) => T | undefined
) {
  if (!manager) throw new Error('实时查询客户端尚未初始化')
  return manager.read(url, key, signal, parse, patch)
}

/** One-shot read of any stream endpoint: the stream closes after the first
 * snapshot regardless of URL or key. Pair with `gcTime: 0` so each new
 * observation (e.g. reopening a picker) takes a fresh snapshot. */
export function readLiveSnapshot<T>(
  url: string,
  key: QueryKey,
  signal?: AbortSignal,
  parse?: (value: unknown) => T
) {
  if (!manager) throw new Error('实时查询客户端尚未初始化')
  return manager.readSnapshot(url, key, signal, parse)
}

/** Standard one-shot query (detail headers, reference lists, the task center):
 * reads one snapshot when first observed, never stays subscribed and is
 * dropped when the view unmounts. After a successful write on the page call
 * `queryClient.invalidateQueries({ queryKey })` to read a fresh snapshot. */
export function snapshotQueryOptions<T>(
  url: string,
  queryKey: QueryKey,
  parse?: (value: unknown) => T
) {
  // Generic factory: callers own the key, which must identify `url`; `parse`
  // is a pure decoder and does not change what is fetched.
  // eslint-disable-next-line @tanstack/query/exhaustive-deps
  return queryOptions({
    queryKey,
    retry: false,
    gcTime: 0,
    queryFn: ({ signal }) => readLiveSnapshot<T>(url, queryKey, signal, parse),
  })
}

/** Reads one named event of a combined stream (e.g. `/api/overview/stream`
 * events `snapshot`, `traffic`, `nodes`) into its own query. All queries on the
 * same URL share one EventSource, opened while any of them is observed and
 * released after the last unmounts. `patch` receives `node-runtime` /
 * `origin-runtime` frames for this event's data. */
export function readLiveEvent<T>(
  url: string,
  event: string,
  key: QueryKey,
  signal?: AbortSignal,
  parse?: (value: unknown) => T,
  patch?: (current: T | undefined, value: unknown) => T | undefined
) {
  if (!manager) throw new Error('实时查询客户端尚未初始化')
  return manager.readEvent(url, event, key, signal, parse, patch)
}

/** Closes live-query streams whose views already unmounted in this commit.
 * Code that opens its own EventSource (live log tails) calls this first, so a
 * tab switch never holds the old and the new stream at once. */
export function releaseDepartedLiveQueries() {
  manager?.releaseDeparted()
}

export function closeLiveQueries() {
  manager?.close()
}
