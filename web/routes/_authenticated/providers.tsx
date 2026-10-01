import { createFileRoute } from '@tanstack/react-router'
import { ProvidersPage } from '@/features/providers'

export const Route = createFileRoute('/_authenticated/providers')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: ProvidersPage,
})
