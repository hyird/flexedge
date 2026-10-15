import {
  useCallback,
  useEffect,
  useRef,
  useState,
  useSyncExternalStore,
} from 'react'
import {
  notifyManager,
  useQueryClient,
  type QueryCache,
} from '@tanstack/react-query'

/** Layout of the outgoing tab panel, read in the event that switches tabs. */
export type TabPanelGeometry = {
  /** Panel height in px. */
  height: number
  /** Scrollable distance left below the scroll container's viewport. */
  below: number
  /** The panel starts inside the scroll container's viewport. */
  startsOnScreen: boolean
}

/** Queries waiting for their first data (a snapshot or stream frame). */
function firstLoadHashes(cache: QueryCache) {
  return cache
    .getAll()
    .filter(
      (query) =>
        query.state.data === undefined && query.state.fetchStatus === 'fetching'
    )
    .map((query) => query.queryHash)
}

type Switching = {
  /** Height that keeps the scroll position once the incoming tab loaded. */
  settledHeight: number | undefined
  /** Queries already waiting for first data before the switch. */
  earlierLoads: Set<string>
}

/** Panel height held across one tab switch.
 *
 * While the incoming tab's first data loads, the panel keeps the outgoing
 * tab's height, so a skeleton, empty state or blank first frame never
 * collapses the page (which would clamp the scroll position and toggle the
 * scrollbar). Once that data is in, the panel only keeps the height that
 * leaves the scroll position where it was, so the header and tab list stay
 * put; when the tabs were already scrolled out of view nothing is kept. */
export class TabPanelHold {
  private minHeight: number | undefined
  private listeners = new Set<() => void>()
  private switching: Switching | null = null
  private stopWatching: (() => void) | null = null

  constructor(private cache: QueryCache) {}

  subscribe = (listener: () => void) => {
    this.listeners.add(listener)
    return () => {
      this.listeners.delete(listener)
      if (this.listeners.size === 0) this.stop()
    }
  }

  getMinHeight = () => this.minHeight

  /** Call in the event that switches tabs, before the switch renders. */
  hold({ height, below, startsOnScreen }: TabPanelGeometry) {
    this.stop()
    const keep = startsOnScreen ? height - below : 0
    this.switching = {
      settledHeight: keep > 0 ? keep : undefined,
      earlierLoads: new Set(firstLoadHashes(this.cache)),
    }
    this.setMinHeight(height)
  }

  /** Call after the incoming tab mounted, i.e. once its queries subscribed
   * and started loading; releases the hold as soon as no query that started
   * loading since `hold` still waits for its first data. */
  watch() {
    const switching = this.switching
    if (!switching || this.stopWatching) return
    // Batched like Query observer notifications, so the release renders
    // together with the incoming data, never before it.
    this.stopWatching = this.cache.subscribe(
      notifyManager.batchCalls(() => this.settleWhenLoaded(switching))
    )
    this.settleWhenLoaded(switching)
  }

  private settleWhenLoaded(switching: Switching) {
    // A batched call may arrive after the next switch replaced this one.
    if (this.switching !== switching) return
    const loading = firstLoadHashes(this.cache).some(
      (hash) => !switching.earlierLoads.has(hash)
    )
    if (loading) return
    this.stop()
    this.setMinHeight(switching.settledHeight)
  }

  private stop() {
    this.stopWatching?.()
    this.stopWatching = null
    this.switching = null
  }

  private setMinHeight(minHeight: number | undefined) {
    if (minHeight === this.minHeight) return
    this.minHeight = minHeight
    this.listeners.forEach((listener) => listener())
  }
}

/** Nearest scrolling ancestor: the page itself, or e.g. a sheet body. */
function scrollContainerOf(element: HTMLElement) {
  for (
    let node = element.parentElement;
    node && node !== document.body;
    node = node.parentElement
  ) {
    const { overflowY } = getComputedStyle(node)
    if (overflowY === 'auto' || overflowY === 'scroll') return node
  }
  return document.scrollingElement ?? document.documentElement
}

function measureTabPanel(panel: HTMLElement): TabPanelGeometry {
  const scroller = scrollContainerOf(panel)
  const box = panel.getBoundingClientRect()
  const viewTop =
    scroller === document.scrollingElement
      ? 0
      : scroller.getBoundingClientRect().top
  return {
    height: box.height,
    below: scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight,
    startsOnScreen: box.top >= viewTop,
  }
}

/** Keeps a tab panel from collapsing or moving the page while tabs switch
 * (see `TabPanelHold`). Attach `ref` and `style` to the `Tabs.Panel` and call
 * `holdForSwitch()` in the same event that changes the selected tab. */
export function useTabPanelHold() {
  const client = useQueryClient()
  const ref = useRef<HTMLDivElement>(null)
  const [hold] = useState(() => new TabPanelHold(client.getQueryCache()))
  const minHeight = useSyncExternalStore(
    hold.subscribe,
    hold.getMinHeight,
    hold.getMinHeight
  )
  // Effects run children first: by now the incoming tab's queries started.
  useEffect(() => {
    hold.watch()
  })
  const holdForSwitch = useCallback(() => {
    if (ref.current) hold.hold(measureTabPanel(ref.current))
  }, [hold])
  return {
    ref,
    style: minHeight === undefined ? undefined : { minHeight },
    holdForSwitch,
  }
}
