import { type ReactNode } from 'react'
import { Tabs } from '@heroui/react'

export type PageTab<T extends string> = {
  id: T
  label: string
  icon?: ReactNode
}

/** In-page tabs of one route. The selected tab lives in the page's `?tab=`
 * search param (default tab omitted); switching calls `onSelectionChange`,
 * which navigates with `replace: true` so the pathname, breadcrumb and
 * history stay unchanged. `children` is the selected tab's content and fills
 * the rest of the page, so a `SaveBar` rendered last still sticks to the
 * bottom. */
export function PageTabs<T extends string>({
  label,
  items,
  selected,
  onSelectionChange,
  children,
}: {
  /** Accessible name of the tab list, e.g. `网站详情`. */
  label: string
  items: readonly PageTab<T>[]
  selected: T
  onSelectionChange: (tab: T) => void
  children: ReactNode
}) {
  return (
    <Tabs
      variant='secondary'
      selectedKey={selected}
      onSelectionChange={(key) => onSelectionChange(key as T)}
      className='-mt-2 flex w-full min-w-0 flex-1 flex-col gap-6'
    >
      <Tabs.ListContainer>
        <Tabs.List aria-label={label}>
          {items.map((item) => (
            <Tabs.Tab
              key={item.id}
              id={item.id}
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
      <Tabs.Panel
        id={selected}
        className='flex min-w-0 flex-1 flex-col gap-6 p-0'
      >
        {children}
      </Tabs.Panel>
    </Tabs>
  )
}
