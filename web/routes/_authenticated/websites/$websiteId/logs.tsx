import { createFileRoute } from '@tanstack/react-router'
import { accessLogsSearchSchema } from '@/features/websites/access-log-search'
import { AccessLogs } from '@/features/websites/access-logs'

export const Route = createFileRoute(
  '/_authenticated/websites/$websiteId/logs'
)({
  validateSearch: accessLogsSearchSchema,
  staticData: { breadcrumb: '访问日志' },
  component: AccessLogs,
})
