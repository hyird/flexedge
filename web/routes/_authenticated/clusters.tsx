import { createFileRoute } from '@tanstack/react-router'
import { Clusters } from '@/features/clusters'

export const Route = createFileRoute('/_authenticated/clusters')({
  validateSearch: (search: Record<string, unknown>) => ({
    view: search.view === 'nodes' ? ('nodes' as const) : ('clusters' as const),
    cluster_id:
      typeof search.cluster_id === 'string' ? search.cluster_id : undefined,
  }),
  component: Clusters,
})
