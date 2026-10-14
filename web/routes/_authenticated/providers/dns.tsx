import { createFileRoute } from '@tanstack/react-router'
import { DnsProvidersPage } from '@/features/providers/dns-providers-page'
import { providersSearchSchema } from '@/features/providers/search'

export const Route = createFileRoute('/_authenticated/providers/dns')({
  validateSearch: providersSearchSchema,
  staticData: { breadcrumb: 'DNS 账号' },
  component: DnsProvidersPage,
})
