import type { ReactNode } from 'react'
import { Loader2, Search } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'

type Props = {
  value?: string
  onChange?: (value: string) => void
  onSearch: () => void
  onReset: () => void
  refreshing?: boolean
  placeholder?: string
  filters?: ReactNode
}

export function ResourceToolbar({
  value,
  onChange,
  onSearch,
  onReset,
  refreshing,
  placeholder = '搜索…',
  filters,
}: Props) {
  return (
    <div className='flex w-full flex-wrap items-center gap-2 rounded-lg border bg-card/70 p-2 shadow-sm'>
      {value !== undefined && (
        <div className='relative min-w-0 flex-1 basis-48 sm:max-w-64'>
          <Search className='absolute start-2.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground' />
          <Input
            value={value}
            onChange={(event) => onChange?.(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Enter') {
                event.preventDefault()
                onSearch()
              }
            }}
            aria-label={placeholder}
            placeholder={placeholder}
            className='ps-8'
          />
        </div>
      )}
      {filters}
      <div className='ms-auto flex items-center gap-2'>
        <Button type='button' variant='outline' onClick={onReset}>
          重置
        </Button>
        <Button
          type='button'
          className='w-[4.5rem]'
          onClick={onSearch}
          disabled={refreshing}
          aria-busy={refreshing}
        >
          {refreshing ? <Loader2 className='animate-spin' aria-hidden='true' /> : '查询'}
        </Button>
      </div>
    </div>
  )
}
