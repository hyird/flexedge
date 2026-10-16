import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { api, getData, pageDataSchema, sendData, streamPath } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import { resourceListSearchQuery } from '@/lib/resource'
import { websitesSnapshotKey } from '@/features/websites/data'
import { certificateSchema, type Certificate } from './types'

const certificateOptionsSchema = z.array(certificateSchema)
const certificatePageSchema = pageDataSchema(certificateSchema)

export const usableCertificateOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.certificates, 'usable-options'],
  queryFn: async ({ signal }) =>
    parseResponseData(
      certificateOptionsSchema,
      await getData<unknown>('/certificates/options', { usable: true }, signal)
    ),
})

export function certificatesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  status?: Certificate['status']
  /** Valid and expiring within 30 days (overview `certificate_expiring_count`). */
  expiring?: 1
  /** Failed/expired, or valid with an error (overview `certificate_failed_count`). */
  abnormal?: 1
}) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.certificates, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/certificates', params),
        queryKey,
        signal,
        (value) => parseResponseData(certificatePageSchema, value)
      ),
  })
}

/** Command palette search of certificates. */
export function certificateSearchQuery(keyword: string) {
  return resourceListSearchQuery(
    '/certificates',
    queryKeys.certificates,
    keyword,
    certificateSchema.pick({ id: true, domains: true }),
    (certificate) => ({
      id: certificate.id,
      label: certificate.domains[0] ?? certificate.id,
    })
  )
}

/** Live certificate detail (`/certificates/:id/stream`). */
export function certificateDetailQuery(certificateId: string) {
  return queryOptions({
    retry: false,
    // The detail page renders not-found and stream errors itself.
    meta: { inlineError: true },
    queryKey: [...queryKeys.certificates, 'detail', certificateId],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/certificates/' + certificateId),
        queryKey,
        signal,
        (value) => parseResponseData(certificateSchema, value)
      ),
  })
}

/** After any certificate write: live list and detail streams push the change,
 * so certificate snapshots (e.g. the website editor's usable certificates)
 * are only marked stale; the snapshot lists of websites referencing
 * certificates re-read. */
export function afterCertificateWrite(client: QueryClient) {
  return Promise.all([
    client.invalidateQueries({
      queryKey: queryKeys.certificates,
      refetchType: 'none',
    }),
    client.invalidateQueries({ queryKey: websitesSnapshotKey }),
  ])
}

type CertificateTarget = Pick<Certificate, 'id' | 'revision'>

export function createCertificate(input: {
  domain: string
  certificate_provider_id: string
  dns_zone_id: string
  config: { auto_renew: boolean }
}) {
  return sendData('post', '/certificates', input)
}

export function updateCertificateRenewal(
  target: CertificateTarget,
  autoRenew: boolean
) {
  return sendData(
    'put',
    '/certificates/' + target.id,
    { auto_renew: autoRenew },
    target.revision
  )
}

export function renewCertificate(target: CertificateTarget) {
  return sendData(
    'post',
    '/certificates/' + target.id + '/renew',
    undefined,
    target.revision
  )
}

export function removeCertificate(target: CertificateTarget) {
  return sendData(
    'delete',
    '/certificates/' + target.id,
    undefined,
    target.revision
  )
}

export async function downloadCertificate(
  target: Pick<Certificate, 'id' | 'domains'>
) {
  const response = await api.get<Blob>(
    '/certificates/' + target.id + '/download',
    { responseType: 'blob' }
  )
  const disposition = response.headers['content-disposition'] as
    string | undefined
  return {
    blob: response.data,
    filename:
      disposition?.match(/filename="([^"]+)"/)?.[1] ??
      (target.domains[0] || 'certificate') + '.zip',
  }
}
