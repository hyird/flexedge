import { useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { Outlet, useLocation, useNavigate } from '@tanstack/react-router'
import {
  ArrowRightFromSquare,
  ChevronDown,
  Globe,
  Moon,
  Sun,
  Display,
} from '@gravity-ui/icons'
import { AppLayout, Navbar, Sheet, Sidebar } from '@heroui-pro/react'
import { Avatar, Button, Dropdown, Label, Tooltip, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { useTheme } from '@/context/theme-context'
import { Confirm } from '@/components/confirm'
import { logout, sessionQueryOptions } from '@/features/auth/data'
import { GlobalSearch } from './global-search'
import { navigation } from './navigation'

export function AppShell() {
  const navigate = useNavigate()
  const { pathname } = useLocation()
  const user = useQuery(sessionQueryOptions).data
  const title =
    navigation.find((item) => item.href === pathname)?.label ?? 'FlexEdge'
  return (
    <>
      <a
        href='#workspace-content'
        className='sr-only focus:not-sr-only focus:fixed focus:top-3 focus:left-3 focus:z-50 focus:rounded-lg focus:bg-surface focus:p-3'
      >
        跳至内容
      </a>
      <AppLayout
        sidebar={
          <>
            <Sidebar>
              <SidebarContents pathname={pathname} />
            </Sidebar>
            <Sidebar.Mobile role='navigation' aria-label='移动导航'>
              <Sheet.Heading className='sr-only'>导航菜单</Sheet.Heading>
              <SidebarContents pathname={pathname} mobile />
            </Sidebar.Mobile>
          </>
        }
        navbar={
          <Navbar
            maxWidth='full'
            size='sm'
            height='var(--workspace-navbar-height)'
          >
            <Navbar.Header>
              <AppLayout.MenuToggle aria-label='打开导航菜单' />
              <Sidebar.Trigger aria-label='展开或收起侧栏' />
              <h1 className='min-w-0 text-lg font-semibold tracking-tight'>
                {pathname === '/'
                  ? `你好，${user?.nickname || user?.username || ''}`
                  : title}
              </h1>
              <Navbar.Spacer />
              <GlobalSearch />
              <ThemeMenu />
            </Navbar.Header>
          </Navbar>
        }
        navigate={(href) => {
          void navigate({ to: href })
        }}
        sidebarCollapsible='icon'
      >
        <div id='workspace-content' className='min-w-0'>
          <Outlet />
        </div>
      </AppLayout>
    </>
  )
}
function SidebarContents({
  pathname,
  mobile = false,
}: {
  pathname: string
  mobile?: boolean
}) {
  const user = useQuery(sessionQueryOptions).data
  const client = useQueryClient()
  const navigate = useNavigate()
  const [signOut, setSignOut] = useState(false)
  return (
    <>
      <Sidebar.Header>
        <div className='flex items-center gap-3 px-1 py-2'>
          <span className='flex size-8 shrink-0 items-center justify-center rounded-xl bg-accent text-accent-foreground'>
            <Globe className='size-5' />
          </span>
          <div data-sidebar='label'>
            <span className='text-sm font-semibold'>FlexEdge</span>
            <p className='text-xs text-muted'>边缘网络控制台</p>
          </div>
        </div>
      </Sidebar.Header>
      <Sidebar.Content>
        {['工作台', '边缘网络', '资源管理', '运维'].map((group) => (
          <Sidebar.Group key={group}>
            <Sidebar.GroupLabel>{group}</Sidebar.GroupLabel>
            <Sidebar.Menu aria-label={group}>
              {navigation
                .filter((item) => item.group === group)
                .map((item) => (
                  <Sidebar.MenuItem
                    key={item.href}
                    href={item.href}
                    id={`${mobile ? 'mobile-' : ''}${item.href}`}
                    isCurrent={pathname === item.href}
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
      <Sidebar.Footer>
        <Dropdown>
          <Button
            variant='ghost'
            className='w-full justify-start px-1 py-3'
            aria-label='账户菜单'
          >
            <Avatar size='sm'>
              <Avatar.Fallback>
                {(user?.nickname || user?.username || 'FE')
                  .slice(0, 2)
                  .toUpperCase()}
              </Avatar.Fallback>
            </Avatar>
            <div className='min-w-0 text-left' data-sidebar='label'>
              <span className='block truncate text-sm font-medium'>
                {user?.nickname || user?.username}
              </span>
              <span className='block text-xs text-muted'>当前账户</span>
            </div>
            <ChevronDown
              className='ml-auto size-4 text-muted'
              data-sidebar='label'
            />
          </Button>
          <Dropdown.Popover placement='top start'>
            <Dropdown.Menu
              aria-label='账户菜单'
              onAction={(key) => {
                if (key === 'logout') setSignOut(true)
              }}
            >
              <Dropdown.Item
                id='identity'
                textValue={user?.username || '当前账户'}
                isDisabled
              >
                <Label>{user?.username}</Label>
              </Dropdown.Item>
              <Dropdown.Item id='logout' textValue='退出登录' variant='danger'>
                <ArrowRightFromSquare className='size-4' />
                <Label>退出登录</Label>
              </Dropdown.Item>
            </Dropdown.Menu>
          </Dropdown.Popover>
        </Dropdown>
      </Sidebar.Footer>
      <Sidebar.Rail aria-label='展开或收起侧栏' />
      {signOut && (
        <Confirm
          title='退出登录'
          description='确认退出当前账户？正在打开的实时视图将关闭。'
          onClose={() => setSignOut(false)}
          onConfirm={async () => {
            try {
              await logout(client)
              await navigate({
                to: '/sign-in',
                search: { redirect: undefined },
                replace: true,
              })
            } catch (error) {
              toast.danger(apiErrorMessage(error))
              throw error
            }
          }}
        />
      )}
    </>
  )
}
function ThemeMenu() {
  const { theme, setTheme } = useTheme()
  const Icon = theme === 'dark' ? Moon : theme === 'system' ? Display : Sun
  return (
    <Dropdown>
      <Tooltip delay={0}>
        <Button size='sm' variant='ghost' isIconOnly aria-label='切换主题'>
          <Icon className='size-4' />
        </Button>
        <Tooltip.Content>切换主题</Tooltip.Content>
      </Tooltip>
      <Dropdown.Popover>
        <Dropdown.Menu
          aria-label='选择主题'
          selectionMode='single'
          selectedKeys={[theme]}
          onAction={(key) => {
            if (key === 'light' || key === 'dark' || key === 'system')
              setTheme(key)
          }}
        >
          {[
            { id: 'light', label: '浅色', icon: Sun },
            { id: 'dark', label: '深色', icon: Moon },
            { id: 'system', label: '跟随系统', icon: Display },
          ].map((item) => (
            <Dropdown.Item id={item.id} key={item.id} textValue={item.label}>
              <item.icon className='size-4' />
              <Label>{item.label}</Label>
              <Dropdown.ItemIndicator />
            </Dropdown.Item>
          ))}
        </Dropdown.Menu>
      </Dropdown.Popover>
    </Dropdown>
  )
}
