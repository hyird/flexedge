import { createFileRoute } from '@tanstack/react-router'
import { certificatesSearchSchema } from '@/features/certificates/certificate-search'
import { CertificatesPage } from '@/features/certificates/certificates-page'

export const Route = createFileRoute('/_authenticated/certificates/')({
  validateSearch: certificatesSearchSchema,
  staticData: { breadcrumb: '证书' },
  component: CertificatesPage,
})
