import { formatDate } from '@/lib/format'
import type { Certificate } from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { StatusBadge } from '@/components/status-badge'

export function CertificateDetailSheet({
  certificate,
  onOpenChange,
}: {
  certificate: Certificate | null
  onOpenChange: (open: boolean) => void
}) {
  return (
    <Sheet open={!!certificate} onOpenChange={onOpenChange}>
      <SheetContent className='w-full overflow-y-auto sm:max-w-xl'>
        <SheetHeader className='text-start'>
          <SheetTitle>{certificate?.domains[0]}</SheetTitle>
          <SheetDescription>证书签发与分发详情。</SheetDescription>
        </SheetHeader>
        {certificate && (
          <div className='space-y-5 px-4 pb-6'>
            <div className='flex flex-wrap gap-2'>
              <StatusBadge status={certificate.status} />
              <Badge variant='outline'>
                {certificate.config.auto_renew ? '自动续期' : '手动续期'}
              </Badge>
              <Badge variant='outline'>
                {certificate.website_count} 个网站使用
              </Badge>
            </div>
            {[
              ['签发机构', certificate.issuer],
              ['有效期开始', formatDate(certificate.not_before)],
              ['有效期结束', formatDate(certificate.expires_at)],
              ['最近签发', formatDate(certificate.last_issued_at)],
              ['序列号', certificate.serial_number || '—'],
              ['SHA-256 指纹', certificate.fingerprint_sha256 || '—'],
            ].map(([label, value]) => (
              <div key={label} className='grid gap-1'>
                <div className='text-xs text-muted-foreground'>{label}</div>
                <div className='text-sm break-all'>{value}</div>
              </div>
            ))}
            {certificate.last_error && (
              <div className='rounded-md border border-destructive/30 bg-destructive/5 p-3 text-sm text-destructive'>
                {certificate.last_error}
              </div>
            )}
          </div>
        )}
      </SheetContent>
    </Sheet>
  )
}

