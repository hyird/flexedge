import { createContext, useContext, useEffect } from 'react'
import { useMatch } from '@tanstack/react-router'
import { navigationItemFor } from './navigation'

declare module '@tanstack/react-router' {
  interface StaticDataRouteOption {
    /** Static breadcrumb title for this route (e.g. `配置`). Resource detail
     * layouts set a generic title and replace it with `useBreadcrumbLabel`. */
    breadcrumb?: string
  }
}

type BreadcrumbMatch = {
  id: string
  pathname: string
  breadcrumb?: string
}
type BreadcrumbCrumb = { key: string; label: string; href?: string }

const normalizePath = (path: string) => path.replace(/\/+$/, '') || '/'

/** Navigation section first (`网站`), then every matched route that declares a
 * breadcrumb, deduplicated by path. Runtime labels from `useBreadcrumbLabel`
 * override static titles. The last crumb is the current page (no link). */
export function buildBreadcrumbTrail(
  pathname: string,
  matches: BreadcrumbMatch[],
  labels: ReadonlyMap<string, string>
): BreadcrumbCrumb[] {
  const section = navigationItemFor(pathname)
  const crumbs: Required<BreadcrumbCrumb>[] = section
    ? [{ key: section.href, label: section.label, href: section.href }]
    : []
  for (const match of matches) {
    const label = labels.get(match.id) ?? match.breadcrumb
    const href = normalizePath(match.pathname)
    if (!label || crumbs.some((crumb) => crumb.href === href)) continue
    crumbs.push({ key: match.id, label, href })
  }
  return crumbs.map((crumb, index) =>
    index === crumbs.length - 1 ? { key: crumb.key, label: crumb.label } : crumb
  )
}

/** Runtime breadcrumb labels keyed by route match id. */
export class BreadcrumbLabelStore {
  private labels: ReadonlyMap<string, string> = new Map()
  private listeners = new Set<() => void>()

  subscribe = (listener: () => void) => {
    this.listeners.add(listener)
    return () => this.listeners.delete(listener)
  }
  snapshot = () => this.labels

  set(matchId: string, label: string | undefined) {
    if (this.labels.get(matchId) === label) return
    const next = new Map(this.labels)
    if (label) next.set(matchId, label)
    else next.delete(matchId)
    this.labels = next
    this.listeners.forEach((notify) => notify())
  }
}

export const BreadcrumbLabelContext =
  createContext<BreadcrumbLabelStore | null>(null)

/** Shows `label` as the breadcrumb of the calling route (e.g. the website name
 * in `/websites/$websiteId`), replacing its static `staticData.breadcrumb`
 * while the component is mounted. Pass `undefined` while data loads. */
export function useBreadcrumbLabel(label: string | undefined) {
  const store = useContext(BreadcrumbLabelContext)
  const matchId = useMatch({ strict: false, select: (match) => match.id })
  useEffect(() => {
    if (!store || !matchId) return
    store.set(matchId, label)
    return () => store.set(matchId, undefined)
  }, [store, matchId, label])
}
