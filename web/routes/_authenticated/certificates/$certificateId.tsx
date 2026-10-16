import { createFileRoute } from '@tanstack/react-router'
import { CertificateDetailPage } from '@/features/certificates/certificate-detail-page'
import { certificateDetailSearchSchema } from '@/features/certificates/certificate-search'

export const Route = createFileRoute(
  '/_authenticated/certificates/$certificateId'
)({
  validateSearch: certificateDetailSearchSchema,
  staticData: { breadcrumb: '证书详情' },
  component: CertificateDetailPage,
})
