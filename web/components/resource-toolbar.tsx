import type { ReactNode } from 'react'
import { RotateCw, Search, X } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'

type Props = {
  value: string
  onChange: (value: string) => void
  onSearch: () => void
  onReset: () => void
  onRefresh: () => void
  refreshing?: boolean
  placeholder?: string
  filters?: ReactNode
  actions?: ReactNode
}

export function ResourceToolbar({
  value,
  onChange,
  onSearch,
  onReset,
  onRefresh,
  refreshing,
  placeholder = '搜索…',
  filters,
  actions,
}: Props) {
  return (
    <div className='flex flex-col gap-2 sm:flex-row sm:items-center'>
      <div className='relative w-full sm:w-64'>
        <Search className='absolute start-2.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground' />
        <Input
          value={value}
          onChange={(event) => onChange(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === 'Enter') onSearch()
          }}
          placeholder={placeholder}
          className='ps-8'
        />
      </div>
      {filters}
      <div className='flex gap-2 sm:ms-auto'>
        {value && (
          <Button variant='ghost' size='sm' onClick={onReset}>
            <X />
            清除
          </Button>
        )}
        <Button
          variant='outline'
          size='icon'
          onClick={onRefresh}
          disabled={refreshing}
          aria-label='刷新数据'
        >
          <RotateCw className={refreshing ? 'animate-spin' : undefined} />
        </Button>
        {actions}
      </div>
    </div>
  )
}
