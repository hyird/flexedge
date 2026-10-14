import { createFileRoute } from '@tanstack/react-router'
import { NodeLogs } from '@/features/nodes/node-logs'

export const Route = createFileRoute('/_authenticated/nodes/$nodeId/logs')({
  staticData: { breadcrumb: '日志' },
  component: NodeLogs,
})
