import type { ReactNode } from 'react'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

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
        <ProfileDropdown />
      </Header>
      <Main fixed={fixed} className='flex flex-1 flex-col gap-3 sm:gap-4'>
        <div className='flex flex-wrap items-end justify-between gap-2'>
          <div>
            <h1 className='text-xl font-semibold tracking-tight'>{title}</h1>
            <p className='text-sm text-muted-foreground'>{description}</p>
          </div>
          {actions && <div className='flex items-center gap-2'>{actions}</div>}
        </div>
        {children}
      </Main>
    </>
  )
}
