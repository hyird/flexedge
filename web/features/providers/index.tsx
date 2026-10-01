import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Tabs, toast } from '@heroui/react'
import { apiErrorMessage, type PageData } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Page } from '@/components/page'
import { ResourceList, ResourceSearch } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { CertificateProviderDialog } from './certificate-provider-dialog'
import {
  certificateProvidersListQuery,
  dnsProvidersQuery,
  removeProvider,
  verifyProvider,
} from './data'
import { DnsProviderDialog } from './dns-provider-dialog'
import { providerLabel } from './provider-display'
import type { CertificateProvider, DnsProvider } from './types'

export function ProvidersPage() {
  const [tab, setTab] = useState('dns')
  return (
    <Page title='服务商' description='管理 DNS 账号与证书签发凭据。'>
      <Tabs selectedKey={tab} onSelectionChange={(key) => setTab(String(key))}>
        <Tabs.ListContainer>
          <Tabs.List aria-label='服务商类型'>
            <Tabs.Tab id='dns'>
              DNS 账号
              <Tabs.Indicator />
            </Tabs.Tab>
            <Tabs.Tab id='certificate'>
              证书供应商
              <Tabs.Indicator />
            </Tabs.Tab>
          </Tabs.List>
        </Tabs.ListContainer>
        <Tabs.Panel id='dns' className='pt-3'>
          {tab === 'dns' && <DnsProviders />}
        </Tabs.Panel>
        <Tabs.Panel id='certificate' className='pt-3'>
          {tab === 'certificate' && <CertificateProviders />}
        </Tabs.Panel>
      </Tabs>
    </Page>
  )
}

function useProviderActions(onRemoved: () => void) {
  const client = useQueryClient()
  const verify = useMutation({
    mutationFn: verifyProvider,
    onSuccess: (response, target) => {
      toast.success(response.message || '验证任务已提交')
      void client.invalidateQueries({
        queryKey: [...queryKeys.providers, target.kind],
      })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const remove = useMutation({
    mutationFn: removeProvider,
    onSuccess: (response, target) => {
      toast.success(response.message || '已删除')
      onRemoved()
      void client.invalidateQueries({
        queryKey: [...queryKeys.providers, target.kind],
      })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  return { verify, remove }
}

function DnsProviders() {
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [dialog, setDialog] = useState<DnsProvider | 'new' | null>(null)
  const [target, setTarget] = useState<DnsProvider | null>(null)
  const query = useQuery(
    dnsProvidersQuery({ page, page_size: size, keyword: keyword || undefined })
  )
  const { verify, remove } = useProviderActions(() => setTarget(null))
  const columns: DataGridColumn<DnsProvider>[] = [
    {
      id: 'name',
      header: '账号',
      isRowHeader: true,
      minWidth: 210,
      cell: (item) => (
        <div>
          <strong className='block font-medium'>{item.name}</strong>
          <span className='text-xs text-muted'>
            {providerLabel(item.provider)}
          </span>
        </div>
      ),
    },
    {
      id: 'account',
      header: '账户标识',
      minWidth: 180,
      cell: (item) => <code className='text-xs'>{item.account_id}</code>,
    },
    {
      id: 'zones',
      header: '托管域名',
      minWidth: 100,
      cell: (item) => (
        <span className='tabular-nums'>{item.zone_count} 个</span>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 110,
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'verified',
      header: '最近验证',
      minWidth: 170,
      cell: (item) => (
        <span className='text-xs text-muted'>
          {formatDate(item.last_verified_at)}
        </span>
      ),
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (item) => (
        <RowMenu
          label={`${item.name}操作`}
          actions={[
            { id: 'edit', label: '编辑', onAction: () => setDialog(item) },
            {
              id: 'verify',
              label: '验证凭据',
              disabled: verify.isPending,
              onAction: () =>
                verify.mutate({
                  kind: 'dns',
                  id: item.id,
                  revision: item.revision,
                }),
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
    <>
      <ResourceList
        label='DNS 服务商账号'
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
              setPage(1)
              setKeyword(draft.trim())
            }}
          >
            <ResourceSearch
              label='搜索账号'
              value={draft}
              onChange={setDraft}
            />
            <Button type='submit' variant='secondary' size='sm'>
              搜索
            </Button>
            {keyword && (
              <Button
                size='sm'
                variant='ghost'
                onPress={() => {
                  setDraft('')
                  setKeyword('')
                  setPage(1)
                }}
              >
                清除
              </Button>
            )}
          </form>
        }
        toolbar={
          <Button size='sm' onPress={() => setDialog('new')}>
            添加账号
          </Button>
        }
        emptyTitle='暂无 DNS 账号'
        emptyDescription='添加 DNS 服务商凭据后即可托管域名。'
      />
      {dialog && (
        <DnsProviderDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          provider={dialog === 'new' ? undefined : dialog}
          onClose={() => setDialog(null)}
        />
      )}
      {target && (
        <Confirm
          title='删除 DNS 账号'
          description={`将删除“${target.name}”。关联域名会阻止此操作。`}
          onClose={() => setTarget(null)}
          busy={remove.isPending}
          onConfirm={() =>
            remove.mutateAsync({
              kind: 'dns',
              id: target.id,
              revision: target.revision,
            })
          }
        />
      )}
    </>
  )
}

function CertificateProviders() {
  const query = useQuery(certificateProvidersListQuery)
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(20)
  const [keyword, setKeyword] = useState('')
  const [dialog, setDialog] = useState<CertificateProvider | 'new' | null>(null)
  const [target, setTarget] = useState<CertificateProvider | null>(null)
  const { verify, remove } = useProviderActions(() => setTarget(null))
  const filtered = (query.data ?? []).filter((item) =>
    `${providerLabel(item.provider)} ${item.account_email ?? ''}`
      .toLowerCase()
      .includes(keyword.toLowerCase())
  )
  const currentPage = Math.min(
    page,
    Math.max(1, Math.ceil(filtered.length / size))
  )
  const data: PageData<CertificateProvider> = {
    list: filtered.slice((currentPage - 1) * size, currentPage * size),
    total: filtered.length,
    page: currentPage,
    page_size: size,
    total_pages: Math.max(1, Math.ceil(filtered.length / size)),
  }
  const columns: DataGridColumn<CertificateProvider>[] = [
    {
      id: 'provider',
      header: '供应商',
      isRowHeader: true,
      minWidth: 170,
      cell: (item) => (
        <strong className='font-medium'>{providerLabel(item.provider)}</strong>
      ),
    },
    {
      id: 'mode',
      header: '接入方式',
      minWidth: 110,
      cell: (item) =>
        item.credential_mode === 'email' ? '邮箱' : 'Access Key',
    },
    {
      id: 'account',
      header: '账户',
      minWidth: 190,
      cell: (item) => item.account_email || item.access_key_hint || '—',
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 110,
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'verified',
      header: '最近验证',
      minWidth: 170,
      cell: (item) => (
        <span className='text-xs text-muted'>
          {formatDate(item.last_verified_at)}
        </span>
      ),
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (item) => (
        <RowMenu
          label={`${providerLabel(item.provider)}操作`}
          actions={[
            { id: 'edit', label: '编辑', onAction: () => setDialog(item) },
            {
              id: 'verify',
              label: '验证凭据',
              disabled: verify.isPending,
              onAction: () =>
                verify.mutate({
                  kind: 'certificate',
                  id: item.id,
                  revision: item.revision,
                }),
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
    <>
      <ResourceList
        label='证书供应商'
        query={{
          isPending: query.isPending,
          isError: query.isError,
          error: query.error,
          isFetching: query.isFetching,
          refetch: query.refetch,
        }}
        data={data}
        columns={columns}
        page={currentPage}
        onPageChange={setPage}
        pageSize={size}
        onPageSizeChange={(value) => {
          setSize(value)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        filters={
          <ResourceSearch
            label='搜索供应商'
            value={keyword}
            onChange={(value) => {
              setKeyword(value)
              setPage(1)
            }}
          />
        }
        toolbar={
          <Button size='sm' onPress={() => setDialog('new')}>
            添加供应商
          </Button>
        }
        emptyTitle='暂无证书供应商'
        emptyDescription='添加签发凭据，为网站申请受信任证书。'
      />
      {dialog && (
        <CertificateProviderDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          provider={dialog === 'new' ? undefined : dialog}
          onClose={() => setDialog(null)}
        />
      )}
      {target && (
        <Confirm
          title='删除证书供应商'
          description={`将删除 ${providerLabel(target.provider)} 凭据。关联证书会阻止此操作。`}
          onClose={() => setTarget(null)}
          busy={remove.isPending}
          onConfirm={() =>
            remove.mutateAsync({
              kind: 'certificate',
              id: target.id,
              revision: target.revision,
            })
          }
        />
      )}
    </>
  )
}
