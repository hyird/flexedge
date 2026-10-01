import { createFileRoute } from '@tanstack/react-router'
import { CertificatesPage } from '@/features/certificates'

export const Route = createFileRoute('/_authenticated/certificates')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: CertificatesPage,
})
