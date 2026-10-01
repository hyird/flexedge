import { createFileRoute } from '@tanstack/react-router'
import { ClustersPage } from '@/features/clusters'

export const Route = createFileRoute('/_authenticated/clusters')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: ClustersPage,
})
