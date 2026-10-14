import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { certificateStatusIds } from '@/features/certificates/certificate-display'
import { CertificatesPage } from '@/features/certificates/certificates-page'

export const Route = createFileRoute('/_authenticated/certificates/')({
  validateSearch: listSearchSchema.extend({
    status: optionalSearchValue(z.enum(certificateStatusIds)),
    expiring: optionalSearchValue(z.literal(1)),
    abnormal: optionalSearchValue(z.literal(1)),
    create: optionalSearchValue(z.literal(1)),
  }),
  staticData: { breadcrumb: '证书' },
  component: CertificatesPage,
})
