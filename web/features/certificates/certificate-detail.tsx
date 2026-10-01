import { formatDate } from '@/lib/format'
import { Dialog, Notice } from '@/components/forms'
import { StatusChip } from '@/components/status-chip'
import type { Certificate } from './types'

export function CertificateDetailDialog({
  certificate,
  onClose,
}: {
  certificate: Certificate
  onClose: () => void
}) {
  return (
    <Dialog
      title={certificate.domains[0] || '证书详情'}
      onClose={onClose}
      size='md'
    >
      <div className='grid gap-5'>
        <div className='flex flex-wrap items-center gap-3'>
          <StatusChip status={certificate.status} />
          <span className='text-xs text-muted'>
            {certificate.config.auto_renew ? '自动续期' : '手动续期'} ·{' '}
            {certificate.website_count} 个关联网站
          </span>
        </div>
        <dl className='grid gap-4 text-sm'>
          {[
            ['证书域名', certificate.domains.join('、')],
            [
              '签发机构',
              certificate.issuer || certificate.certificate_provider,
            ],
            ['DNS 验证域名', certificate.dns_zone_domain],
            ['有效期开始', formatDate(certificate.not_before)],
            ['有效期结束', formatDate(certificate.expires_at)],
            ['最近签发', formatDate(certificate.last_issued_at)],
            ['序列号', certificate.serial_number || '—'],
            ['SHA-256 指纹', certificate.fingerprint_sha256 || '—'],
          ].map(([label, value]) => (
            <div key={label}>
              <dt className='text-xs text-muted'>{label}</dt>
              <dd className='mt-1 break-all'>{value}</dd>
            </div>
          ))}
        </dl>
        {certificate.sync_status && (
          <div className='flex items-center gap-3 text-sm'>
            <span className='text-muted'>节点分发</span>
            <StatusChip status={certificate.sync_status} />
            {certificate.sync_count_fails ? (
              <span className='text-danger'>
                {certificate.sync_count_fails} 次失败
              </span>
            ) : null}
          </div>
        )}
        {certificate.last_error && <Notice>{certificate.last_error}</Notice>}
      </div>
    </Dialog>
  )
}
