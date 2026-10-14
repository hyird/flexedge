import { type ReactNode } from 'react'
import { useLocation } from '@tanstack/react-router'
import { Tabs } from '@heroui/react'

type RouteTab = {
  /** Concrete in-app path, e.g. `/websites/${id}/traffic`. */
  href: string
  label: string
  icon?: ReactNode
}

/** Index tabs share the parent path, so the longest matching href wins. */
function activeRouteTab(items: RouteTab[], pathname: string) {
  const path = pathname.replace(/\/+$/, '') || '/'
  return items
    .filter((item) => {
      const href = item.href.replace(/\/+$/, '') || '/'
      return path === href || path.startsWith(`${href}/`)
    })
    .sort((left, right) => right.href.length - left.href.length)[0]?.href
}

/** Tabs whose items are child routes. Selection follows the URL, so back/forward
 * and copied links restore the tab; render `<Outlet />` below this component. */
export function RouteTabs({
  items,
  label,
}: {
  items: RouteTab[]
  /** Accessible name of the tab list, e.g. `网站详情`. */
  label: string
}) {
  const { pathname } = useLocation()
  return (
    <Tabs
      variant='secondary'
      selectedKey={activeRouteTab(items, pathname) ?? null}
      className='-mt-2 w-full min-w-0'
    >
      <Tabs.ListContainer>
        <Tabs.List aria-label={label}>
          {items.map((item) => (
            <Tabs.Tab
              key={item.href}
              id={item.href}
              href={item.href}
              className='w-auto flex-none px-3'
            >
              <span className='flex items-center gap-1.5 [&_svg]:size-4'>
                {item.icon}
                {item.label}
              </span>
              <Tabs.Indicator />
            </Tabs.Tab>
          ))}
        </Tabs.List>
      </Tabs.ListContainer>
    </Tabs>
  )
}
