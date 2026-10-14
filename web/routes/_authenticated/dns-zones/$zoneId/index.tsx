import { createFileRoute } from '@tanstack/react-router'
import { ZoneOverview } from '@/features/dns-zones/zone-overview'

export const Route = createFileRoute('/_authenticated/dns-zones/$zoneId/')({
  component: ZoneOverview,
})
