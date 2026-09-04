import { createFileRoute } from '@tanstack/react-router'
import { Nodes } from '@/features/nodes'

export const Route = createFileRoute('/_authenticated/nodes')({
  validateSearch: (search: Record<string, unknown>) => ({
    cluster_id:
      typeof search.cluster_id === 'string' ? search.cluster_id : undefined,
  }),
  component: Nodes,
})
