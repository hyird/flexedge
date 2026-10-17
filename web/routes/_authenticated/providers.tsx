import { createFileRoute } from '@tanstack/react-router'
import { ProvidersPage } from '@/features/providers/providers_page'
import { providersSearchSchema } from '@/features/providers/provider-search'

export const Route = createFileRoute('/_authenticated/providers')({
  validateSearch: providersSearchSchema,
  staticData: { breadcrumb: '服务商' },
  component: ProvidersPage,
})
