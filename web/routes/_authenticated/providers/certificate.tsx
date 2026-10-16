import { createFileRoute } from '@tanstack/react-router'
import { CertificateProvidersPage } from '@/features/providers/certificate-providers-page'
import { providersSearchSchema } from '@/features/providers/provider-search'

export const Route = createFileRoute('/_authenticated/providers/certificate')({
  validateSearch: providersSearchSchema,
  staticData: { breadcrumb: '证书' },
  component: CertificateProvidersPage,
})
