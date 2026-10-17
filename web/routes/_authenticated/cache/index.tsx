import { createFileRoute } from '@tanstack/react-router'
import { CachePage } from '@/features/cache/cache_page'
import { cacheSearchSchema } from '@/features/cache/cache_search'

export const Route = createFileRoute('/_authenticated/cache/')({
  validateSearch: cacheSearchSchema,
  staticData: { breadcrumb: '缓存' },
  component: CachePage,
})
