import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Choice } from '@/components/forms'
import { Page } from '@/components/page'
import { ResourceList, ResourceSearch } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { CertificateDetailDialog } from './certificate-detail'
import { CertificateDialog } from './certificate-dialog'
import {
  certificatesQuery,
  downloadCertificate,
  removeCertificate,
  renewCertificate,
} from './data'
import type { Certificate } from './types'

export function CertificatesPage() {
  const client = useQueryClient()
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [status, setStatus] = useState('all')
  const [dialog, setDialog] = useState<Certificate | 'new' | null>(null)
  const [detail, setDetail] = useState<Certificate | null>(null)
  const [target, setTarget] = useState<Certificate | null>(null)
  const query = useQuery(
    certificatesQuery({
      page,
      page_size: size,
      keyword: keyword || undefined,
      status: status === 'all' ? undefined : status,
    })
  )
  const renew = useMutation({
    mutationFn: renewCertificate,
    onSuccess: (response) => {
      toast.success(response.message || '续期任务已提交')
      void client.invalidateQueries({ queryKey: queryKeys.certificates })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const remove = useMutation({
    mutationFn: removeCertificate,
    onSuccess: (response) => {
      toast.success(response.message || '已删除')
      setTarget(null)
      void client.invalidateQueries({ queryKey: queryKeys.certificates })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const download = useMutation({
    mutationFn: downloadCertificate,
    onSuccess: ({ blob, filename }) => {
      const url = URL.createObjectURL(blob)
      const anchor = document.createElement('a')
      anchor.href = url
      anchor.download = filename
      document.body.append(anchor)
      anchor.click()
      anchor.remove()
      setTimeout(() => URL.revokeObjectURL(url), 1000)
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const columns: DataGridColumn<Certificate>[] = [
    {
      id: 'domains',
      header: '证书域名',
      isRowHeader: true,
      minWidth: 230,
      cell: (item) => (
        <div>
          <Button
            size='sm'
            variant='ghost'
            className='justify-start px-0 font-medium'
            onPress={() => setDetail(item)}
          >
            {item.domains[0]}
          </Button>
          {item.domains.length > 1 && (
            <p className='text-xs text-muted'>
              另含 {item.domains.length - 1} 个域名
            </p>
          )}
        </div>
      ),
    },
    {
      id: 'issuer',
      header: '签发机构',
      minWidth: 150,
      cell: (item) => item.issuer || item.certificate_provider,
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 120,
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'expire',
      header: '有效期至',
      minWidth: 170,
      cell: (item) => (
        <div>
          <span className='text-xs'>{formatDate(item.expires_at)}</span>
          {item.remaining_days !== undefined && (
            <p className='text-xs text-muted tabular-nums'>
              剩余 {item.remaining_days} 天
            </p>
          )}
        </div>
      ),
    },
    {
      id: 'renewal',
      header: '续期',
      minWidth: 90,
      cell: (item) => (item.config.auto_renew ? '自动续期' : '手动续期'),
    },
    {
      id: 'websites',
      header: '关联网站',
      minWidth: 100,
      cell: (item) => (
        <span className='tabular-nums'>{item.website_count} 个</span>
      ),
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (item) => (
        <RowMenu
          label={`${item.domains[0]}证书操作`}
          actions={[
            {
              id: 'detail',
              label: '查看详情',
              onAction: () => setDetail(item),
            },
            {
              id: 'settings',
              label: '续期设置',
              onAction: () => setDialog(item),
            },
            {
              id: 'renew',
              label: '立即续期',
              disabled: renew.isPending,
              onAction: () => renew.mutate(item),
            },
            {
              id: 'download',
              label: '下载证书',
              disabled: !item.usable || download.isPending,
              onAction: () => download.mutate(item),
            },
            {
              id: 'delete',
              label: '删除',
              danger: true,
              onAction: () => setTarget(item),
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='证书'
      description='通过 DNS 验证申请证书，管理有效期与自动续期。'
      actions={
        <Button size='sm' onPress={() => setDialog('new')}>
          申请证书
        </Button>
      }
    >
      <ResourceList
        label='证书'
        query={query}
        columns={columns}
        page={page}
        onPageChange={setPage}
        pageSize={size}
        onPageSizeChange={(value) => {
          setSize(value)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        filters={
          <form
            className='flex flex-wrap items-end gap-2'
            onSubmit={(event) => {
              event.preventDefault()
              setKeyword(draft.trim())
              setPage(1)
            }}
          >
            <ResourceSearch
              label='搜索证书'
              value={draft}
              onChange={setDraft}
            />
            <Choice
              compact
              label='证书状态'
              value={status}
              onChange={(value) => {
                setStatus(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部状态' },
                { id: 'pending', label: '待签发' },
                { id: 'issuing', label: '签发中' },
                { id: 'valid', label: '已签发' },
                { id: 'renewing', label: '续期中' },
                { id: 'expired', label: '已过期' },
                { id: 'failed', label: '失败' },
              ]}
            />
            <Button size='sm' type='submit' variant='secondary'>
              搜索
            </Button>
            <Button
              size='sm'
              variant='ghost'
              onPress={() => {
                setDraft('')
                setKeyword('')
                setStatus('all')
                setPage(1)
              }}
            >
              重置
            </Button>
          </form>
        }
        emptyTitle='暂无证书'
        emptyDescription='添加证书供应商和 DNS 托管域名后，即可申请证书。'
      />
      {dialog && (
        <CertificateDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          certificate={dialog === 'new' ? undefined : dialog}
          onClose={() => setDialog(null)}
        />
      )}
      {detail && (
        <CertificateDetailDialog
          certificate={
            query.data?.list.find((item) => item.id === detail.id) ?? detail
          }
          onClose={() => setDetail(null)}
        />
      )}
      {target && (
        <Confirm
          title='删除证书'
          description={`确定删除“${target.domains.join('、')}”？关联网站会阻止此操作。`}
          busy={remove.isPending}
          onClose={() => setTarget(null)}
          onConfirm={() => remove.mutateAsync(target)}
        />
      )}
    </Page>
  )
}
