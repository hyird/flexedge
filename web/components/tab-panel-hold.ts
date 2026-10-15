import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  useSyncExternalStore,
  type CSSProperties,
} from 'react'
import {
  notifyManager,
  useQueryClient,
  type QueryCache,
} from '@tanstack/react-query'
import { loadingEpoch } from '@/lib/minimum-loading'

/** Layout of the outgoing tab panel, read in the event that switches tabs. */
export type TabPanelGeometry = {
  /** Panel height in px. */
  height: number
  /** Scrollable distance left below the scroll container's viewport. */
  below: number
  /** The panel starts inside the scroll container's viewport. */
  startsOnScreen: boolean
}

/** Inline style of the held panel. */
export type TabPanelHoldStyle = Pick<
  CSSProperties,
  'minHeight' | 'maxHeight' | 'overflow'
>

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
  earlierQueries: Set<string>
  /** Loads outside Query (`useTabPanelLoading`) registered before it. */
  earlierLoads: Set<object>
}

/** Panel height held across one tab switch.
 *
 * While the incoming tab's first data loads, the panel keeps exactly the
 * outgoing tab's height: a skeleton, empty state or blank first frame can
 * neither collapse the page (which would clamp the scroll position) nor grow
 * it past the outgoing height (a placeholder taller than both tabs' content),
 * so the page's scroll height, and with it the scrollbar, changes at most
 * once per switch, when the real content renders. The placeholder beyond that
 * height is clipped meanwhile; it starts at or below the bottom of the
 * outgoing tab's content. First data is a Query's first snapshot or stream
 * frame, or a load reported through `useTabPanelLoading` (a live log tail's
 * first batch). Once all of it rendered, the panel only keeps the height that
 * leaves the scroll position where it was, so the header and tab list stay
 * put; when the tabs were already scrolled out of view nothing is kept. */
export class TabPanelHold {
  private style: TabPanelHoldStyle | undefined
  private listeners = new Set<() => void>()
  private switching: Switching | null = null
  private stopWatching: (() => void) | null = null
  private loads = new Set<object>()

  constructor(private cache: QueryCache) {}

  subscribe = (listener: () => void) => {
    this.listeners.add(listener)
    return () => {
      this.listeners.delete(listener)
      if (this.listeners.size === 0) this.stop()
    }
  }

  getStyle = () => this.style

  /** Call in the event that switches tabs, before the switch renders. */
  hold({ height, below, startsOnScreen }: TabPanelGeometry) {
    this.stop()
    const keep = startsOnScreen ? height - below : 0
    this.switching = {
      settledHeight: keep > 0 ? keep : undefined,
      earlierQueries: new Set(firstLoadHashes(this.cache)),
      earlierLoads: new Set(this.loads),
    }
    this.setStyle({ minHeight: height, maxHeight: height, overflow: 'clip' })
  }

  /** Registers content that loads its first data outside Query; returns
   * the call that reports it loaded (or failed). */
  addLoad() {
    const load = {}
    this.loads.add(load)
    return () => {
      this.loads.delete(load)
      this.requestSettle()
    }
  }

  /** Call after every commit of the panel's owner. Children's effects ran
   * first, so the incoming tab's queries started and its other loads are
   * registered, including loads that only begin once an earlier one landed
   * (a log tail opening after the header snapshot). Releases the hold when
   * nothing that started loading since `hold` still waits for first data. */
  settle() {
    const switching = this.switching
    if (!switching) return
    if (!this.stopWatching) {
      // Batched like Query observer notifications: the owner re-renders in
      // the same commit as the incoming data, and this check runs after it.
      this.stopWatching = this.cache.subscribe(
        notifyManager.batchCalls(() => {
          if (this.switching === switching && !this.queriesLoading(switching))
            this.requestSettle()
        })
      )
    }
    if (this.queriesLoading(switching) || this.otherLoading(switching)) return
    this.stop()
    this.setStyle(
      switching.settledHeight === undefined
        ? undefined
        : { minHeight: switching.settledHeight }
    )
  }

  private queriesLoading(switching: Switching) {
    return firstLoadHashes(this.cache).some(
      (hash) => !switching.earlierQueries.has(hash)
    )
  }

  private otherLoading(switching: Switching) {
    for (const load of this.loads)
      if (!switching.earlierLoads.has(load)) return true
    return false
  }

  /** Re-renders the owner so `settle` runs after the current commit. */
  private requestSettle() {
    if (!this.switching) return
    this.setStyle({ ...this.style })
  }

  private stop() {
    this.stopWatching?.()
    this.stopWatching = null
    this.switching = null
  }

  private setStyle(style: TabPanelHoldStyle | undefined) {
    if (style === this.style) return
    this.style = style
    this.listeners.forEach((listener) => listener())
  }
}

/** The hold of the enclosing tab panel; `useTabPanelHold` owners provide it
 * around the panel content. */
export const TabPanelHoldContext = createContext<TabPanelHold | null>(null)

/** Reports that content inside a tab panel is loading its first data outside
 * TanStack Query (e.g. a live log tail before its first batch), so a switch
 * to its tab keeps the panel's height until that data renders too. */
export function useTabPanelLoading(loading: boolean) {
  const hold = useContext(TabPanelHoldContext)
  // Layout effect: registered within the commit that mounts the content,
  // before the owner's `settle` runs; released within the data's commit.
  useLayoutEffect(
    () => (hold && loading ? hold.addLoad() : undefined),
    [hold, loading]
  )
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

/** Keeps a tab panel from collapsing, growing or moving the page while tabs
 * switch (see `TabPanelHold`). Attach `ref` and `style` to the `Tabs.Panel`,
 * provide `hold` through `TabPanelHoldContext` around the panel content and
 * call `holdForSwitch()` in the same event that changes the selected tab;
 * it also begins the switch's loading epoch, so the incoming tab's first
 * loads reveal together once the switch is `minimumLoadingMs` old. */
export function useTabPanelHold() {
  const client = useQueryClient()
  const ref = useRef<HTMLDivElement>(null)
  const [hold] = useState(() => new TabPanelHold(client.getQueryCache()))
  const style = useSyncExternalStore(
    hold.subscribe,
    hold.getStyle,
    hold.getStyle
  )
  // Effects run children first: by now the incoming tab's loads started.
  useEffect(() => {
    hold.settle()
  })
  const holdForSwitch = useCallback(() => {
    loadingEpoch.begin()
    if (ref.current) hold.hold(measureTabPanel(ref.current))
  }, [hold])
  return { ref, style, holdForSwitch, hold }
}
