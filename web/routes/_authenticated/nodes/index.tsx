import { createFileRoute } from '@tanstack/react-router'
import { nodesSearchSchema } from '@/features/nodes/node-search'
import { NodesPage } from '@/features/nodes/nodes-page'

export const Route = createFileRoute('/_authenticated/nodes/')({
  validateSearch: nodesSearchSchema,
  staticData: { breadcrumb: '节点' },
  component: NodesPage,
})
