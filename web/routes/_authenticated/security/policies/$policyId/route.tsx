import { createFileRoute, Outlet } from '@tanstack/react-router'

export const Route = createFileRoute(
  '/_authenticated/security/policies/$policyId'
)({ staticData: { breadcrumb: '规则集' }, component: Outlet })
