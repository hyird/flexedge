import { useMemo, useState } from 'react'
import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { Download, Eye, Pencil, Plus, RefreshCw, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { api, getData, sendData } from '@/lib/api'
import { formatDate } from '@/lib/format'
import type {
  Certificate,
  CertificateProvider,
  DnsZoneOption,
  PageData,
} from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import {
  Form,
  FormControl,
  FormDescription,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'

const createSchema = z.object({
  domain: z
    .string()
    .trim()
    .min(1, '请输入证书域名')
    .max(253)
    .regex(/^(?:\*\.)?([A-Za-z0-9-]+\.)+[A-Za-z]{2,63}$/, '域名格式不正确'),
  certificate_provider_id: z.string().uuid('请选择证书供应商'),
  dns_zone_id: z.string().uuid('请选择托管域名'),
  auto_renew: z.boolean(),
})

type CreateValues = z.infer<typeof createSchema>

export function Certificates() {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [draftKeyword, setDraftKeyword] = useState('')
  const [keyword, setKeyword] = useState('')
  const [status, setStatus] = useState('all')
  const [dialog, setDialog] = useState<Certificate | 'new' | null>(null)
  const [detailTarget, setDetailTarget] = useState<Certificate | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Certificate | null>(null)

  const query = useQuery({
    queryKey: ['certificates', page, pageSize, keyword, status],
    queryFn: () =>
      getData<PageData<Certificate>>('/certificates/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        status: status === 'all' ? undefined : status,
      }),
  })
  const { mutate: renewCertificate } = useMutation({
    mutationFn: (item: Certificate) =>
      sendData(
        'post',
        `/certificates/${item.id}/renew`,
        undefined,
        item.revision
      ),
    onSuccess: async (response) => {
      toast.success(response.message)
      await queryClient.invalidateQueries({ queryKey: ['certificates'] })
    },
  })
  const remove = useMutation({
    mutationFn: (item: Certificate) =>
      sendData('delete', `/certificates/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: ['certificates'] })
    },
  })

  const download = async (item: Certificate) => {
    const response = await api.get<Blob>(`/certificates/${item.id}/download`, {
      responseType: 'blob',
    })
    const disposition = response.headers['content-disposition'] as
      string | undefined
    const filename =
      disposition?.match(/filename="([^"]+)"/)?.[1] ??
      `${item.domains[0] || 'certificate'}.zip`
    const url = URL.createObjectURL(response.data)
    const link = document.createElement('a')
    link.href = url
    link.download = filename
    link.click()
    URL.revokeObjectURL(url)
  }

  const columns = useMemo<ColumnDef<Certificate>[]>(
    () => [
      {
        id: 'domain',
        accessorFn: (item) => item.domains[0],
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='证书域名' />
        ),
        cell: ({ row }) => (
          <div>
            <div className='font-medium'>{row.original.domains[0]}</div>
            {row.original.domains.length > 1 && (
              <div className='text-xs text-muted-foreground'>
                另有 {row.original.domains.length - 1} 个域名
              </div>
            )}
          </div>
        ),
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => (
          <div className='space-y-1'>
            <StatusBadge status={row.original.status} />
            <div className='text-xs text-muted-foreground'>
              {row.original.usable ? '可用于网站' : '暂不可用'}
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'certificate_provider',
        header: '签发机构',
      },
      {
        accessorKey: 'dns_zone_domain',
        header: 'DNS 验证域名',
      },
      {
        accessorKey: 'expires_at',
        header: '到期时间',
        cell: ({ row }) => (
          <div>
            <div className='whitespace-nowrap'>
              {formatDate(row.original.expires_at)}
            </div>
            <div className='text-xs text-muted-foreground'>
              {row.original.remaining_days === undefined
                ? '—'
                : `剩余 ${row.original.remaining_days} 天`}
            </div>
          </div>
        ),
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDetailTarget(row.original)}>
              <Eye /> 查看详情
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setDialog(row.original)}>
              <Pencil /> 续期设置
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => renewCertificate(row.original)}>
              <RefreshCw /> 重新签发
            </DropdownMenuItem>
            <DropdownMenuItem
              disabled={!row.original.usable}
              onSelect={() => download(row.original)}
            >
              <Download /> 下载证书
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() => setRemoveTarget(row.original)}
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [renewCertificate]
  )

  return (
    <FeatureShell
      title='证书'
      description='申请、自动续期并下载边缘网站证书。'
      actions={
        <Button onClick={() => setDialog('new')}>
          <Plus /> 申请证书
        </Button>
      }
    >
      <ResourceToolbar
        value={draftKeyword}
        onChange={setDraftKeyword}
        onSearch={() => {
          setKeyword(draftKeyword.trim())
          setPage(1)
        }}
        onReset={() => {
          setDraftKeyword('')
          setKeyword('')
          setPage(1)
        }}
        onRefresh={() => query.refetch()}
        refreshing={query.isFetching}
        placeholder='搜索证书域名…'
        filters={
          <Select
            value={status}
            onValueChange={(value) => {
              setStatus(value)
              setPage(1)
            }}
          >
            <SelectTrigger className='w-36'>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value='all'>全部状态</SelectItem>
              <SelectItem value='pending'>等待中</SelectItem>
              <SelectItem value='issuing'>签发中</SelectItem>
              <SelectItem value='valid'>有效</SelectItem>
              <SelectItem value='renewing'>续签中</SelectItem>
              <SelectItem value='failed'>失败</SelectItem>
              <SelectItem value='expired'>已过期</SelectItem>
            </SelectContent>
          </Select>
        }
      />
      <ResourceTable
        columns={columns}
        data={query.data?.list ?? []}
        loading={query.isLoading}
        error={query.isError}
        onRetry={() => void query.refetch()}
        page={page}
        pageSize={pageSize}
        totalPages={query.data?.total_pages ?? 1}
        onPaginationChange={(nextPage, nextSize) => {
          setPage(nextPage)
          setPageSize(nextSize)
        }}
        emptyTitle='暂无证书'
        emptyDescription='申请证书后，签发进度会显示在这里。'
      />
      {dialog && (
        <CertificateDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          certificate={dialog === 'new' ? undefined : dialog}
          open
          onOpenChange={(open) => !open && setDialog(null)}
        />
      )}
      <CertificateDetailSheet
        certificate={detailTarget}
        onOpenChange={(open) => !open && setDetailTarget(null)}
      />
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除证书'
        desc={`确定删除“${removeTarget?.domains[0] ?? ''}”证书吗？有关联网站时服务端会拒绝删除。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}

function CertificateDialog({
  certificate,
  open,
  onOpenChange,
}: {
  certificate?: Certificate
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
  const providersQuery = useQuery({
    queryKey: ['providers', 'certificate'],
    queryFn: () => getData<CertificateProvider[]>('/providers/certificate'),
    enabled: !certificate,
  })
  const zonesQuery = useQuery({
    queryKey: ['dns-zones', 'options'],
    queryFn: () =>
      getData<{ list: DnsZoneOption[] }>('/dns-zones/options').then(
        (data) => data.list
      ),
    enabled: !certificate,
  })
  const form = useForm<CreateValues>({
    resolver: zodResolver(createSchema),
    defaultValues: {
      domain: certificate?.domains[0] ?? '',
      certificate_provider_id: certificate?.certificate_provider_id ?? '',
      dns_zone_id: certificate?.dns_zone_id ?? '',
      auto_renew: certificate?.config.auto_renew ?? true,
    },
  })
  const mutation = useMutation({
    mutationFn: (values: CreateValues) =>
      certificate
        ? sendData(
            'put',
            `/certificates/${certificate.id}`,
            { auto_renew: values.auto_renew },
            certificate.revision
          )
        : sendData('post', '/certificates/', {
            domain: values.domain,
            certificate_provider_id: values.certificate_provider_id,
            dns_zone_id: values.dns_zone_id,
            config: { auto_renew: values.auto_renew },
          }),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: ['certificates'] })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto'>
        <SheetHeader>
          <SheetTitle>{certificate ? '续期设置' : '申请证书'}</SheetTitle>
          <SheetDescription>
            使用 DNS-01 验证申请证书，任务将在后台执行。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='certificate-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='domain'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>证书域名</FormLabel>
                  <FormControl>
                    <Input
                      disabled={!!certificate}
                      placeholder='*.example.com'
                      {...field}
                    />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            {!certificate && (
              <>
                <FormField
                  control={form.control}
                  name='certificate_provider_id'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>证书供应商</FormLabel>
                      <Select
                        value={field.value}
                        onValueChange={field.onChange}
                      >
                        <FormControl>
                          <SelectTrigger>
                            <SelectValue placeholder='选择供应商' />
                          </SelectTrigger>
                        </FormControl>
                        <SelectContent>
                          {providersQuery.data?.map((provider) => (
                            <SelectItem key={provider.id} value={provider.id}>
                              {provider.provider === 'letsencrypt'
                                ? "Let's Encrypt"
                                : 'ZeroSSL'}
                            </SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='dns_zone_id'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>DNS 验证域名</FormLabel>
                      <Select
                        value={field.value}
                        onValueChange={field.onChange}
                      >
                        <FormControl>
                          <SelectTrigger>
                            <SelectValue placeholder='选择托管域名' />
                          </SelectTrigger>
                        </FormControl>
                        <SelectContent>
                          {zonesQuery.data?.map((zone) => (
                            <SelectItem key={zone.id} value={zone.id}>
                              {zone.domain}
                            </SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                      <FormMessage />
                    </FormItem>
                  )}
                />
              </>
            )}
            <FormField
              control={form.control}
              name='auto_renew'
              render={({ field }) => (
                <FormItem className='flex items-start gap-3 rounded-md border p-4'>
                  <FormControl>
                    <Checkbox
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                  <div>
                    <FormLabel>自动续期</FormLabel>
                    <FormDescription>
                      到期前自动提交续签任务并分发到关联网站。
                    </FormDescription>
                  </div>
                </FormItem>
              )}
            />
          </form>
        </Form>
        <SheetFooter>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='certificate-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在提交…' : certificate ? '保存' : '申请'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}

function CertificateDetailSheet({
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
