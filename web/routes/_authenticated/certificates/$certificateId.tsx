import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { optionalSearchValue } from '@/lib/list-search'
import { CertificateDetailPage } from '@/features/certificates/certificate-detail-page'
import { websitesSearchSchema } from '@/features/websites/websites-search'

export const Route = createFileRoute(
  '/_authenticated/certificates/$certificateId'
)({
  // The referencing-websites table keeps its search, filters and page in the URL.
  validateSearch: websitesSearchSchema.extend({
    renewal: optionalSearchValue(z.literal(1)),
  }),
  staticData: { breadcrumb: '证书详情' },
  component: CertificateDetailPage,
})
