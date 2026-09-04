import { createFileRoute } from '@tanstack/react-router'
import { DnsZones } from '@/features/dns-zones'

export const Route = createFileRoute('/_authenticated/dns-zones')({
  component: DnsZones,
})
