import { createFileRoute, redirect } from '@tanstack/react-router'

export const Route = createFileRoute('/_authenticated/')({
  beforeLoad: () => {
    throw redirect({
      to: '/clusters',
      search: { view: 'clusters', cluster_id: undefined },
      replace: true,
    })
  },
})
