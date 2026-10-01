import { createFileRoute } from '@tanstack/react-router'
import { DnsZonesPage } from '@/features/dns-zones'

export const Route = createFileRoute('/_authenticated/dns-zones')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: DnsZonesPage,
})
