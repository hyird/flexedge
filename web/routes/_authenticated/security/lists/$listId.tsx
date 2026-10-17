import { createFileRoute } from '@tanstack/react-router'
import { SecurityListDetail } from '@/features/security/lists_pages'

export const Route = createFileRoute('/_authenticated/security/lists/$listId')({
  staticData: { breadcrumb: '清单' },
  component: SecurityListDetail,
})
