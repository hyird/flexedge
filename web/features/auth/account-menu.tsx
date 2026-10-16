import { useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { ArrowRightFromSquare, ChevronsExpandUpRight } from '@gravity-ui/icons'
import { Avatar, Button, Dropdown, Label, Separator } from '@heroui/react'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { themeOptions, useTheme } from '@/components/theme-context'
import { logout, sessionQueryOptions } from './data'

/** Sidebar footer: current account, theme submenu and sign-out. */
export function AccountMenu() {
  const user = useQuery(sessionQueryOptions).data
  const client = useQueryClient()
  const navigate = useNavigate()
  const { theme, setTheme } = useTheme()
  const [isSigningOut, setSigningOut] = useState(false)
  const name = user?.nickname || user?.username || ''
  return (
    <>
      <Dropdown>
        <Button
          variant='ghost'
          aria-label={`账户菜单：${name}`}
          className='h-auto w-full justify-start gap-2.5 px-1.5 py-1.5'
        >
          <Avatar size='sm' className='shrink-0'>
            <Avatar.Fallback className='text-xs font-semibold'>
              {(name || 'FE').slice(0, 2).toUpperCase()}
            </Avatar.Fallback>
          </Avatar>
          <span
            className='min-w-0 flex-1 truncate text-left text-sm font-medium'
            data-sidebar='label'
          >
            {name}
          </span>
          <ChevronsExpandUpRight
            className='size-4 shrink-0 text-muted'
            data-sidebar='label'
          />
        </Button>
        <Dropdown.Popover placement='top start' className='min-w-56'>
          <Dropdown.Menu
            aria-label='账户菜单'
            onAction={(key) => {
              if (key === 'logout') setSigningOut(true)
            }}
          >
            <Dropdown.Item id='identity' textValue={name} isDisabled>
              <Label>{name}</Label>
              {user?.nickname && user.nickname !== user.username && (
                <span className='ml-auto text-muted'>{user.username}</span>
              )}
            </Dropdown.Item>
            <Separator />
            <Dropdown.SubmenuTrigger>
              <Dropdown.Item id='theme' textValue='主题'>
                <Label>主题</Label>
                <span className='ml-auto text-xs text-muted'>
                  {themeOptions.find((option) => option.id === theme)?.label}
                </span>
                <Dropdown.SubmenuIndicator />
              </Dropdown.Item>
              <Dropdown.Popover>
                <Dropdown.Menu
                  aria-label='选择主题'
                  selectionMode='single'
                  selectedKeys={[theme]}
                  onAction={(key) => {
                    const option = themeOptions.find((item) => item.id === key)
                    if (option) setTheme(option.id)
                  }}
                >
                  {themeOptions.map((option) => (
                    <Dropdown.Item
                      key={option.id}
                      id={option.id}
                      textValue={option.label}
                    >
                      <option.icon className='size-4 text-muted' />
                      <Label>{option.label}</Label>
                      <Dropdown.ItemIndicator />
                    </Dropdown.Item>
                  ))}
                </Dropdown.Menu>
              </Dropdown.Popover>
            </Dropdown.SubmenuTrigger>
            <Separator />
            <Dropdown.Item id='logout' textValue='退出登录' variant='danger'>
              <ArrowRightFromSquare className='size-4' />
              <Label>退出登录</Label>
            </Dropdown.Item>
          </Dropdown.Menu>
        </Dropdown.Popover>
      </Dropdown>
      <ConfirmDialog
        isOpen={isSigningOut}
        onClose={() => setSigningOut(false)}
        title='退出当前账户？'
        description='退出后将关闭所有实时连接，需要重新登录才能继续操作。'
        confirmLabel='退出登录'
        tone='accent'
        onConfirm={async () => {
          await logout(client)
          await navigate({
            to: '/sign-in',
            search: { redirect: undefined },
            replace: true,
          })
        }}
      />
    </>
  )
}
