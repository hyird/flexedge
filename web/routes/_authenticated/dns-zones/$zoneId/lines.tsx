import { createFileRoute } from '@tanstack/react-router'
import { ZoneLines } from '@/features/dns-zones/zone-lines'

export const Route = createFileRoute('/_authenticated/dns-zones/$zoneId/lines')(
  {
    staticData: { breadcrumb: '解析线路' },
    component: ZoneLines,
  }
)
