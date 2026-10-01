import { createFileRoute } from '@tanstack/react-router'
import { CachePoliciesPage } from '@/features/cache-policies'

export const Route = createFileRoute('/_authenticated/cache-policies')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: CachePoliciesPage,
})
