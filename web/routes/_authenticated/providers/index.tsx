import { createFileRoute } from '@tanstack/react-router'
import { ProvidersLayout } from '@/features/providers/providers-layout'
import { providersSearchSchema } from '@/features/providers/search'

export const Route = createFileRoute('/_authenticated/providers/')({
  validateSearch: providersSearchSchema,
  staticData: { breadcrumb: '供应商账号' },
  component: ProvidersLayout,
})
