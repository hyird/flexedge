import type { ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import {
  ArrowDownToLine,
  ArrowLeft,
  ArrowsRotateRight,
  Gear,
  ShieldCheck,
  TrashBin,
} from '@gravity-ui/icons'
import { Alert, Button, Label, ProgressBar } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { useListSearch } from '@/lib/list-search'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { CopyButton } from '@/components/copy-button'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import { WebsitesTable } from '@/features/websites/websites-table'
import { useCertificateCommands } from './certificate-commands'
import {
  certificateStatusLabels,
  certificateSyncStatusLabels,
  formatFingerprint,
  isCertificateNotFound,
  validityProgress,
} from './certificate-display'
import { certificateDetailQuery } from './data'
import { RenewalSettingsSheet } from './renewal-settings-sheet'
import type { Certificate } from './types'

const route = getRouteApi('/_authenticated/certificates/$certificateId')

export function CertificateDetailPage() {
  const { certificateId } = route.useParams()
  const search = route.useSearch()
  const navigate = route.useNavigate()
  const query = useQuery(certificateDetailQuery(certificateId))
  const certificate = query.data
  useBreadcrumbLabel(certificate?.domains[0])
  const commands = useCertificateCommands({
    onRemoved: () => void navigate({ to: '/certificates', replace: true }),
  })
  if (!certificate) {
    if (query.isError && isCertificateNotFound(query.error))
      return (
        <Page>
          <EmptyState
            title='证书不存在'
            description='该证书可能已被删除，或链接有误。'
            action={<LinkButton href='/certificates'>返回证书</LinkButton>}
          />
        </Page>
      )
    return (
      <Page>
        <QueryNotice
          query={query}
          onRetry={() => void query.refetch()}
          title='证书加载失败'
        />
        {query.isPending && <DetailHeaderSkeleton />}
      </Page>
    )
  }
  // Keep the websites table's search params while toggling the sheet.
  const setRenewalOpen = (open: boolean) =>
    void navigate({
      search: (previous) => ({
        ...previous,
        renewal: open ? (1 as const) : undefined,
      }),
      replace: true,
    })
  const busy =
    certificate.status === 'issuing' || certificate.status === 'renewing'
  const providerName = providerLabel(certificate.certificate_provider)
  return (
    <Page>
      <QueryNotice
        query={query}
        onRetry={() => void query.refetch()}
        title='实时连接中断，显示的是最后一次收到的数据'
      />
      <DetailHeader
        icon={<ShieldCheck />}
        title={certificate.domains[0] ?? certificate.id}
        status={
          <StatusChip
            status={certificate.status}
            reserve={certificateStatusLabels}
          />
        }
        meta={[
          certificate.issuer || providerName,
          certificate.config.auto_renew ? '自动续期' : '手动续期',
          // Always shown (`—` before issuance) so the meta line keeps its
          // shape while the certificate is issued or renewed.
          <span key='expires'>
            到期 <RelativeTime value={certificate.expires_at} />
          </span>,
        ]}
        actions={
          <>
            <LinkButton href='/certificates'>
              <ArrowLeft className='size-4' />
              返回
            </LinkButton>
            <Button
              size='sm'
              variant='danger-soft'
              onPress={() => commands.remove(certificate)}
            >
              <TrashBin />
              删除证书
            </Button>
          </>
        }
      />
      {certificate.last_error && (
        <Alert status='danger'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>最近一次签发失败</Alert.Title>
            <Alert.Description>{certificate.last_error}</Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <div className='grid gap-6 lg:grid-cols-3'>
        <ValiditySection
          certificate={certificate}
          actions={
            <>
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => setRenewalOpen(true)}
              >
                <Gear className='size-4' />
                续期设置
              </Button>
              <Button
                size='sm'
                variant='tertiary'
                isDisabled={busy}
                onPress={() => commands.renew(certificate)}
              >
                <ArrowsRotateRight className='size-4' />
                立即续期
              </Button>
            </>
          }
        />
        <Section title='分发状态' description='证书文件下发到边缘节点的进度。'>
          <DescriptionList
            columns={1}
            items={[
              {
                label: '节点分发',
                value: certificate.sync_status ? (
                  <StatusChip
                    status={certificate.sync_status}
                    reserve={certificateSyncStatusLabels}
                  />
                ) : (
                  '—'
                ),
              },
              {
                label: '失败次数',
                value: (
                  <span
                    className={
                      certificate.sync_count_fails
                        ? 'text-danger tabular-nums'
                        : 'tabular-nums'
                    }
                  >
                    {certificate.sync_count_fails ?? 0}
                  </span>
                ),
              },
              {
                label: '可绑定到网站',
                value: certificate.usable ? '是' : '否（尚未签发或已过期）',
              },
            ]}
          />
        </Section>
      </div>
      <div className='grid gap-6 lg:grid-cols-3'>
        <Section title={`域名 · ${certificate.domains.length}`}>
          <ul className='flex flex-col gap-1'>
            {certificate.domains.map((domain) => (
              <li
                key={domain}
                className='flex min-w-0 items-center justify-between gap-2'
              >
                <Mono truncate>{domain}</Mono>
                <CopyButton value={domain} label={`复制 ${domain}`} />
              </li>
            ))}
          </ul>
        </Section>
        <Section
          title='签发信息'
          className='lg:col-span-2'
          contentClassName='flex flex-col gap-4'
          actions={
            <Button
              size='sm'
              variant='tertiary'
              isDisabled={!certificate.usable}
              isPending={commands.downloadingId === certificate.id}
              onPress={() => commands.download(certificate)}
            >
              <ArrowDownToLine className='size-4' />
              下载
            </Button>
          }
        >
          <DescriptionList
            items={[
              {
                label: '签发机构',
                value: certificate.issuer || providerName,
              },
              {
                label: '证书供应商',
                value: providerName,
              },
              {
                label: '托管域名',
                value: <Mono>{certificate.dns_zone_domain}</Mono>,
              },
              {
                label: '最近签发',
                value: formatDate(certificate.last_issued_at),
              },
              {
                label: '序列号',
                value: certificate.serial_number ? (
                  <span className='flex items-center gap-1'>
                    <Mono className='break-all'>
                      {certificate.serial_number}
                    </Mono>
                    <CopyButton
                      value={certificate.serial_number}
                      label='复制序列号'
                    />
                  </span>
                ) : (
                  '—'
                ),
              },
              { label: '申请时间', value: formatDate(certificate.created_at) },
              {
                label: 'SHA-256 指纹',
                wide: true,
                value: certificate.fingerprint_sha256 ? (
                  <span className='flex items-start gap-1'>
                    <Mono className='break-all'>
                      {formatFingerprint(certificate.fingerprint_sha256)}
                    </Mono>
                    <CopyButton
                      value={certificate.fingerprint_sha256}
                      label='复制指纹'
                    />
                  </span>
                ) : (
                  '—'
                ),
              },
            ]}
          />
        </Section>
      </div>
      <ReferencingWebsites certificate={certificate} />
      <RenewalSettingsSheet
        certificate={certificate}
        isOpen={search.renewal === 1}
        onClose={() => setRenewalOpen(false)}
      />
      {commands.dialogs}
    </Page>
  )
}

function ValiditySection({
  certificate,
  actions,
}: {
  certificate: Certificate
  actions: ReactNode
}) {
  const progress = validityProgress(certificate)
  return (
    <Section title='有效期' className='lg:col-span-2' actions={actions}>
      <div className='flex flex-col gap-5'>
        {/* Always rendered so the card keeps its height while the
         * certificate is issued, renewed or expires. */}
        <ProgressBar
          aria-label='剩余有效期'
          value={progress?.percent ?? 0}
          color={progress?.tone ?? 'default'}
          valueLabel={
            progress
              ? `${progress.remainingDays} / ${progress.totalDays} 天`
              : '—'
          }
        >
          <Label>
            {progress
              ? `剩余 ${progress.remainingDays} 天`
              : certificate.status === 'expired'
                ? '证书已过期'
                : '证书签发完成后显示有效期'}
          </Label>
          <ProgressBar.Output className='text-[13px] text-muted tabular-nums' />
          <ProgressBar.Track>
            <ProgressBar.Fill />
          </ProgressBar.Track>
        </ProgressBar>
        <DescriptionList
          columns={3}
          items={[
            { label: '生效时间', value: formatDate(certificate.not_before) },
            { label: '到期时间', value: formatDate(certificate.expires_at) },
            {
              label: '续期方式',
              value: certificate.config.auto_renew
                ? '到期前自动续期'
                : '手动续期',
            },
          ]}
        />
      </div>
    </Section>
  )
}

/** Websites binding this certificate: the shared website list filtered by
 * `certificate_id` on the server, with its search and paging in the URL.
 * A one-shot snapshot (the page keeps a single live stream, the
 * certificate's); the toolbar refresh and this page's writes re-read it. */
function ReferencingWebsites({ certificate }: { certificate: Certificate }) {
  const list = useListSearch(route)
  return (
    <Section
      title={`引用网站 · ${certificate.website_count}`}
      description='在 HTTPS 配置中绑定了此证书的网站，续期后自动更新。'
    >
      <WebsitesTable
        list={list}
        certificateId={certificate.id}
        snapshot
        variant='secondary'
      />
    </Section>
  )
}
