import type { ReactNode } from 'react'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ConfigDrawer } from '@/components/config-drawer'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'
import { ThemeSwitch } from '@/components/theme-switch'

type Props = {
  title: string
  description: string
  actions?: ReactNode
  children: ReactNode
  fixed?: boolean
}

export function FeatureShell({
  title,
  description,
  actions,
  children,
  fixed = false,
}: Props) {
  return (
    <>
      <Header fixed>
        <Search className='me-auto' placeholder='搜索页面…' />
        <ThemeSwitch />
        <ConfigDrawer />
        <ProfileDropdown />
      </Header>
      <Main fixed={fixed} className='flex flex-1 flex-col gap-4 sm:gap-6'>
        <div className='flex flex-wrap items-end justify-between gap-3'>
          <div>
            <h1 className='text-2xl font-bold tracking-tight'>{title}</h1>
            <p className='text-muted-foreground'>{description}</p>
          </div>
          {actions && <div className='flex items-center gap-2'>{actions}</div>}
        </div>
        {children}
      </Main>
    </>
  )
}
