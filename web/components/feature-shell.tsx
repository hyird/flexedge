import type { ReactNode } from 'react'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'
import { ThemeSwitch } from '@/components/theme-switch'
import { TaskCenter } from '@/features/tasks/task-center'

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
        <TaskCenter />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main fixed={fixed} className='flex flex-1 flex-col gap-4 sm:gap-5'>
        <div className='flex flex-wrap items-start justify-between gap-4'>
          <div className='min-w-0'>
            <h1 className='text-2xl font-semibold tracking-tight'>{title}</h1>
            <p className='mt-1 max-w-2xl text-sm leading-6 text-muted-foreground'>
              {description}
            </p>
          </div>
          {actions && (
            <div className='flex shrink-0 items-center gap-2'>{actions}</div>
          )}
        </div>
        {children}
      </Main>
    </>
  )
}
