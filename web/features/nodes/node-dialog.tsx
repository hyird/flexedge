import { z } from 'zod'
import { useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Plus, X } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { Cluster, DnsZone, Node } from '@/lib/types'
import { Button } from '@/components/ui/button'
import {
  Form,
  FormControl,
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
import { DnsLineSelect } from '@/components/dns-line-tree'

const endpointSchema = z.object({
  id: z.string().uuid(),
  ip_address: z.string().trim().min(1, '请输入 IP 地址').max(45),
  line_code: z.string().trim().min(1, '请输入线路代码').max(64),
})

const schema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  name: z.string().trim().min(1, '请输入节点名称').max(100),
  status: z.enum(['enabled', 'disabled']),
  endpoints: z.array(endpointSchema).min(1, '至少配置一个 IP').max(8),
})

type Values = z.infer<typeof schema>
export type NodeCredentials = { node_id: string; secret: string; revision: number }

export function NodeDialog({
  node,
  clusters,
  initialClusterId,
  open,
  onOpenChange,
  onCredentials,
}: {
  node?: Node
  clusters: Cluster[]
  initialClusterId?: string
  open: boolean
  onOpenChange: (open: boolean) => void
  onCredentials: (credentials: NodeCredentials) => void
}) {
  const queryClient = useQueryClient()
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      cluster_id: node?.cluster_id ?? initialClusterId ?? '',
      name: node?.name ?? '',
      status: (node?.status as Values['status']) ?? 'enabled',
      endpoints: node?.config.endpoints ?? [
        {
          id: crypto.randomUUID(),
          ip_address: '',
          line_code: 'default',
        },
      ],
    },
  })
  const clusterId = form.watch('cluster_id')
  const selectedCluster = clusters.find((cluster) => cluster.id === clusterId)
  const linesQuery = useQuery({
    queryKey: [...queryKeys.dnsZones, 'lines', selectedCluster?.dns_zone_id],
    queryFn: () =>
      getData<DnsZone>(`/dns-zones/${selectedCluster!.dns_zone_id}`).then(
        (zone) => zone.runtime.lines
      ),
    enabled: !!selectedCluster?.dns_zone_id,
  })
  const endpoints = useFieldArray({
    control: form.control,
    name: 'endpoints',
    keyName: 'formKey',
  })
  const mutation = useMutation({
    mutationFn: (values: Values) => {
      const body = {
        cluster_id: values.cluster_id,
        name: values.name,
        status: values.status,
        config: { endpoints: values.endpoints },
      }
      return node
        ? sendData('put', `/nodes/${node.id}`, body, node.revision)
        : sendData<NodeCredentials>('post', '/nodes/', body)
    },
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      if (!node && response.data) onCredentials(response.data as NodeCredentials)
      await queryClient.invalidateQueries({ queryKey: queryKeys.nodes })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto sm:max-w-2xl'>
        <SheetHeader>
          <SheetTitle>{node ? '编辑节点' : '添加节点'}</SheetTitle>
          <SheetDescription>
            每个节点可配置最多 8 个唯一 IP 与 DNS 线路。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='node-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <div className='grid items-start gap-4 sm:grid-cols-3'>
              <FormField
                control={form.control}
                name='name'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>节点名称</FormLabel>
                    <FormControl>
                      <Input placeholder='edge-tpe-01' {...field} />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <FormField
                control={form.control}
                name='cluster_id'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>所属集群</FormLabel>
                    <Select value={field.value} onValueChange={field.onChange}>
                      <FormControl>
                        <SelectTrigger>
                          <SelectValue placeholder='选择集群' />
                        </SelectTrigger>
                      </FormControl>
                      <SelectContent>
                        {clusters.map((cluster) => (
                          <SelectItem key={cluster.id} value={cluster.id}>
                            {cluster.name}
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
                name='status'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>状态</FormLabel>
                    <Select value={field.value} onValueChange={field.onChange}>
                      <FormControl>
                        <SelectTrigger>
                          <SelectValue />
                        </SelectTrigger>
                      </FormControl>
                      <SelectContent>
                        <SelectItem value='enabled'>启用</SelectItem>
                        <SelectItem value='disabled'>停用</SelectItem>
                      </SelectContent>
                    </Select>
                  </FormItem>
                )}
              />
            </div>
            <div className='space-y-3'>
              <div className='flex items-center justify-between'>
                <div>
                  <FormLabel>Endpoint</FormLabel>
                  <p className='text-xs text-muted-foreground'>
                    配置节点对外提供服务的 IP。
                  </p>
                </div>
                <Button
                  type='button'
                  variant='outline'
                  size='sm'
                  disabled={endpoints.fields.length >= 8}
                  onClick={() =>
                    endpoints.append({
                      id: crypto.randomUUID(),
                      ip_address: '',
                      line_code: 'default',
                    })
                  }
                >
                  <Plus /> 添加 IP
                </Button>
              </div>
              {endpoints.fields.map((endpoint, index) => (
                <div
                  key={endpoint.formKey}
                  className='grid items-start gap-3 rounded-md border p-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_auto]'
                >
                  <FormField
                    control={form.control}
                    name={`endpoints.${index}.ip_address`}
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel className='sr-only'>IP 地址</FormLabel>
                        <FormControl>
                          <Input placeholder='203.0.113.10' {...field} />
                        </FormControl>
                        <FormMessage />
                      </FormItem>
                    )}
                  />
                  <FormField
                    control={form.control}
                    name={`endpoints.${index}.line_code`}
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel className='sr-only'>DNS线路</FormLabel>
                        <FormControl>
                          <DnsLineSelect
                            lines={linesQuery.data ?? []}
                            value={field.value}
                            onValueChange={field.onChange}
                          />
                        </FormControl>
                        <FormMessage />
                      </FormItem>
                    )}
                  />
                  <Button
                    type='button'
                    variant='ghost'
                    size='icon'
                    aria-label='移除 Endpoint'
                    disabled={endpoints.fields.length === 1}
                    onClick={() => endpoints.remove(index)}
                  >
                    <X />
                  </Button>
                </div>
              ))}
            </div>
          </form>
        </Form>
        <SheetFooter className='mt-0'>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button type='submit' form='node-form' disabled={mutation.isPending}>
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
