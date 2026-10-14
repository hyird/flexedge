import { type ReactNode } from 'react'
import { Tabs } from '@heroui/react'
import {
  TabLeaveGuardContext,
  useTabSwitchGuard,
  type TabSwitchGuard,
} from '@/lib/unsaved-changes'
import { UnsavedChangesDialog } from './save-bar'

export type PageTab<T extends string> = {
  id: T
  label: string
  icon?: ReactNode
}

/** In-page tabs of one route. The selected tab is component state (seeded
 * from the arrival request via `useArrivalState`), so switching never
 * changes the URL, the breadcrumb or the history. `children` is the selected
 * tab's content and fills the rest of the page, so a `SaveBar` rendered last
 * still sticks to the bottom; while that save bar is dirty, switching tabs
 * asks for confirmation first. Pass `guard` (`useTabSwitchGuard` from `@/lib/unsaved-changes`) when
 * controls outside the tabs also switch tabs, and switch through
 * `guard.request(() => …)` there. */
export function PageTabs<T extends string>({
  label,
  items,
  selected,
  onSelectionChange,
  guard,
  children,
}: {
  /** Accessible name of the tab list, e.g. `网站详情`. */
  label: string
  items: readonly PageTab<T>[]
  selected: T
  onSelectionChange: (tab: T) => void
  guard?: TabSwitchGuard
  children: ReactNode
}) {
  const ownGuard = useTabSwitchGuard()
  const active = guard ?? ownGuard
  return (
    <Tabs
      variant='secondary'
      selectedKey={selected}
      onSelectionChange={(key) => {
        if (key !== selected) active.request(() => onSelectionChange(key as T))
      }}
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
        <TabLeaveGuardContext value={active.dirtyRef}>
          {children}
        </TabLeaveGuardContext>
        <UnsavedChangesDialog
          guard={{
            isBlocked: active.isPending,
            stay: active.cancel,
            leave: active.confirm,
          }}
        />
      </Tabs.Panel>
    </Tabs>
  )
}
