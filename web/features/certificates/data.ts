import { queryOptions } from '@tanstack/react-query'
import { api, sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { Certificate } from './types'

export const usableCertificateOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.certificates, 'usable-options'],
  queryFn: ({ queryKey, signal }) => readLiveQuery<Certificate[]>(streamPath('/certificates/options', { usable: true }), queryKey, signal),
})

export function certificatesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  status?: string
}) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.certificates, 'list', params],
    queryFn: ({ queryKey, signal }) => readLiveQuery<PageData<Certificate>>(streamPath('/certificates', params), queryKey, signal),
  })
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
