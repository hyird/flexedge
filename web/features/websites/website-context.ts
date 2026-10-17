import { createContext, useContext } from 'react'
import type { HistoryState } from '@tanstack/react-router'
import type { Website } from './types'

/** Starting values for a tab opened from inside the detail page (e.g. the
 * settings group); same shape as the create page's arrival request. */
export type WebsiteArrival = NonNullable<HistoryState['website']>
export type WebsiteDetailTab = 'overview' | 'traffic' | 'logs' | 'settings'

type WebsiteDetailContext = {
  /** Live detail on the overview/settings tabs, a one-shot snapshot elsewhere. */
  website: Website
  /** Switches the in-page tab without touching the URL; `initial` seeds the
   * opened tab (e.g. the settings group). */
  openTab: (tab: WebsiteDetailTab, initial?: WebsiteArrival) => void
  /** Called after a confirmed delete, before navigating away: the stream's
   * "not found" then no longer replaces the page. */
  markDeleted: () => void
}

/** Provided by the detail layout after the first frame, so the value is
 * present for the header actions and the selected tab. */
export const WebsiteContext = createContext<WebsiteDetailContext | null>(null)

function useDetailContext() {
  const context = useContext(WebsiteContext)
  if (!context) throw new Error('useWebsite 只能在网站详情布局内使用')
  return context
}

export function useWebsite() {
  return useDetailContext().website
}

export function useOpenWebsiteTab() {
  return useDetailContext().openTab
}

export function useMarkWebsiteDeleted() {
  return useDetailContext().markDeleted
}
