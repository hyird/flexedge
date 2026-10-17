import { createFileRoute, Outlet } from '@tanstack/react-router'
import { SecurityDataProvider } from '@/features/security/security_provider'
import { securitySearchSchema } from '@/features/security/security_search'

export const Route = createFileRoute('/_authenticated/security')({
  validateSearch: securitySearchSchema,
  staticData: { breadcrumb: '安全' },
  component: () => (
    <SecurityDataProvider>
      <Outlet />
    </SecurityDataProvider>
  ),
})
