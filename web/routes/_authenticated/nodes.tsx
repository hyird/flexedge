import { createFileRoute, redirect } from '@tanstack/react-router'

export const Route = createFileRoute('/_authenticated/nodes')({
  validateSearch: (search: Record<string, unknown>) => ({
    cluster_id:
      typeof search.cluster_id === 'string' ? search.cluster_id : undefined,
  }),
  beforeLoad: ({ search }) => {
    throw redirect({
      to: '/clusters',
      search: { view: 'nodes', cluster_id: search.cluster_id },
      replace: true,
    })
  },
})
