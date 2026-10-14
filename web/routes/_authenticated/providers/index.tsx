import { createFileRoute } from '@tanstack/react-router'
import { ProvidersLayout } from '@/features/providers/providers-layout'

export const Route = createFileRoute('/_authenticated/providers/')({
  staticData: { breadcrumb: '供应商账号' },
  component: ProvidersLayout,
})
