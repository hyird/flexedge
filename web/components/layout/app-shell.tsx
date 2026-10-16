import { type ReactNode, useState } from 'react'
import { Outlet, useLocation } from '@tanstack/react-router'
import { Globe } from '@gravity-ui/icons'
import { AppLayout, Navbar, Sheet, Sidebar } from '@heroui-pro/react'
import { Tooltip } from '@heroui/react'
import { AppBreadcrumbs } from './app-breadcrumbs'
import {
  BreadcrumbLabelContext,
  BreadcrumbLabelStore,
} from './breadcrumb-trail'
import { CommandPalette, type ResourceSearchSource } from './command-palette'
import { navigation, navigationGroups, navigationItemFor } from './navigation'
import { ThemeMenu } from './theme-menu'

/** Authenticated workspace: collapsible sidebar (sheet on mobile), navbar with
 * breadcrumbs, search, task center and theme menu. The route assembling it
 * passes the business parts: the account menu, the task center and the
 * command palette's resource searches. In-app links route through the React
 * Aria RouterProvider installed by the root route. */
export function AppShell({
  account,
  taskCenter,
  resourceSearch,
}: {
  account: ReactNode
  taskCenter: ReactNode
  resourceSearch: ResourceSearchSource[]
}) {
  const [breadcrumbLabels] = useState(() => new BreadcrumbLabelStore())
  return (
    <BreadcrumbLabelContext.Provider value={breadcrumbLabels}>
      <a
        href='#workspace-content'
        className='sr-only focus:not-sr-only focus:fixed focus:top-3 focus:left-3 focus:z-50 focus:rounded-lg focus:bg-surface focus:p-3'
      >
        跳至内容
      </a>
      <AppLayout
        sidebarVariant='inset'
        sidebarCollapsible='icon'
        sidebar={
          <>
            <Sidebar className='[--sidebar-width:12rem]'>
              <SidebarNavigation account={account} />
              <Sidebar.Rail aria-label='展开或收起侧栏' />
            </Sidebar>
            <Sidebar.Mobile role='navigation' aria-label='导航菜单'>
              <Sheet.Heading className='sr-only'>导航菜单</Sheet.Heading>
              <SidebarNavigation account={account} idPrefix='mobile-' />
            </Sidebar.Mobile>
          </>
        }
        navbar={
          <Navbar
            maxWidth='full'
            size='sm'
            height='var(--workspace-navbar-height)'
          >
            <Navbar.Header className='gap-2 px-3 sm:px-4'>
              <AppLayout.MenuToggle aria-label='打开导航菜单' />
              <Tooltip>
                <Sidebar.Trigger aria-label='展开或收起侧栏' />
                <Tooltip.Content>展开或收起侧栏</Tooltip.Content>
              </Tooltip>
              <div className='min-w-0 flex-1'>
                <AppBreadcrumbs />
              </div>
              <Navbar.Content className='gap-1'>
                <CommandPalette resourceSearch={resourceSearch} />
                {taskCenter}
                <ThemeMenu />
              </Navbar.Content>
            </Navbar.Header>
          </Navbar>
        }
      >
        <div id='workspace-content' className='min-w-0'>
          <Outlet />
        </div>
      </AppLayout>
    </BreadcrumbLabelContext.Provider>
  )
}

function SidebarNavigation({
  account,
  idPrefix = '',
}: {
  account: ReactNode
  idPrefix?: string
}) {
  const { pathname } = useLocation()
  const current = navigationItemFor(pathname)
  return (
    <>
      <Sidebar.Header>
        <div className='flex items-center gap-2.5 px-1 py-1.5'>
          <span className='flex size-8 shrink-0 items-center justify-center rounded-lg bg-accent text-accent-foreground'>
            <Globe className='size-4' />
          </span>
          <span className='flex min-w-0 flex-col' data-sidebar='label'>
            <span className='text-sm leading-5 font-semibold'>FlexEdge</span>
            <span className='text-xs leading-4 text-muted'>边缘控制台</span>
          </span>
        </div>
      </Sidebar.Header>
      <Sidebar.Content>
        {navigationGroups.map((group) => (
          <Sidebar.Group key={group}>
            <Sidebar.GroupLabel>{group}</Sidebar.GroupLabel>
            <Sidebar.Menu aria-label={group}>
              {navigation
                .filter((item) => item.group === group)
                .map((item) => (
                  <Sidebar.MenuItem
                    key={item.href}
                    id={`${idPrefix}${item.href}`}
                    href={item.href}
                    isCurrent={current?.href === item.href}
                    textValue={item.label}
                  >
                    <Sidebar.MenuIcon>
                      <item.icon className='size-4' />
                    </Sidebar.MenuIcon>
                    <Sidebar.MenuLabel>{item.label}</Sidebar.MenuLabel>
                  </Sidebar.MenuItem>
                ))}
            </Sidebar.Menu>
          </Sidebar.Group>
        ))}
      </Sidebar.Content>
      <Sidebar.Footer>{account}</Sidebar.Footer>
    </>
  )
}
