import { createContext, useContext } from 'react'
import type { HistoryState } from '@tanstack/react-router'
import type { Website } from './types'

/** Arrival request for the detail page (`state.website`). */
export type WebsiteArrival = NonNullable<HistoryState['website']>
export type WebsiteDetailTab = NonNullable<WebsiteArrival['tab']>

type WebsiteDetailContext = {
  /** Live detail on the overview/settings tabs, a one-shot snapshot elsewhere. */
  website: Website
  /** Re-reads the header snapshot after a write on a snapshot tab; a no-op
   * where the live detail stream already pushes the change. */
  refresh: () => void
  /** Switches the in-page tab without touching the URL; `initial` seeds the
   * opened tab (e.g. the settings group). */
  openTab: (tab: WebsiteDetailTab, initial?: WebsiteArrival) => void
}

/** Provided by the detail layout, which renders tabs only after the first
 * frame, so the value is always present inside a tab. */
export const WebsiteContext = createContext<WebsiteDetailContext | null>(null)

function useDetailContext() {
  const context = useContext(WebsiteContext)
  if (!context) throw new Error('useWebsite 只能在网站详情布局内使用')
  return context
}

export function useWebsite() {
  return useDetailContext().website
}

export function useRefreshWebsite() {
  return useDetailContext().refresh
}

export function useOpenWebsiteTab() {
  return useDetailContext().openTab
}
