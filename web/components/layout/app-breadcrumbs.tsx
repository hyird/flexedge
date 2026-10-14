import { useContext, useSyncExternalStore } from 'react'
import { useLocation, useMatches } from '@tanstack/react-router'
import { Breadcrumbs } from '@heroui/react'
import {
  BreadcrumbLabelContext,
  buildBreadcrumbTrail,
} from './breadcrumb-trail'

const emptyLabels: ReadonlyMap<string, string> = new Map()
const noSubscription = () => () => undefined

/** Navbar breadcrumbs built from the navigation section and route matches. */
export function AppBreadcrumbs() {
  const { pathname } = useLocation()
  const store = useContext(BreadcrumbLabelContext)
  const labels = useSyncExternalStore(
    store?.subscribe ?? noSubscription,
    store?.snapshot ?? (() => emptyLabels)
  )
  const matches = useMatches({
    select: (all) =>
      all.map((match) => ({
        id: match.id,
        pathname: match.pathname,
        breadcrumb: match.staticData.breadcrumb,
      })),
    structuralSharing: true,
  })
  const crumbs = buildBreadcrumbTrail(pathname, matches, labels)
  return (
    <Breadcrumbs aria-label='面包屑' className='min-w-0 overflow-hidden'>
      {crumbs.map((crumb) => (
        <Breadcrumbs.Item
          key={crumb.key}
          href={crumb.href}
          className='min-w-0 last:font-semibold last:text-foreground'
        >
          <span className='block max-w-[16rem] truncate'>{crumb.label}</span>
        </Breadcrumbs.Item>
      ))}
    </Breadcrumbs>
  )
}
