import { createFileRoute } from '@tanstack/react-router'
import { SecurityCenter } from '@/features/security/security_center'

export const Route = createFileRoute('/_authenticated/security/')({
  staticData: { breadcrumb: '安全' },
  component: SecurityCenter,
})
