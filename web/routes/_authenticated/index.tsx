import { createFileRoute } from '@tanstack/react-router'
import { OverviewPage } from '@/features/overview'

export const Route = createFileRoute('/_authenticated/')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: OverviewPage,
})
