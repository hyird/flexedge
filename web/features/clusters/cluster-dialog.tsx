import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery } from '@tanstack/react-query'
import { toast } from 'sonner'
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
import type { Cluster } from '@/features/clusters/types'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { saveCluster } from './data'

const schema = z.object({
  name: z.string().trim().min(1, '请输入集群名称').max(100),
  dns_zone_id: z.string().uuid('请选择托管域名'),
  hostname_prefix: z
    .string()
    .trim()
    .min(1, '请输入主机前缀')
    .max(63)
    .regex(
      /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/,
      '仅支持字母、数字和连字符'
    ),
  status: z.enum(['enabled', 'disabled']),
})

type Values = z.infer<typeof schema>

export function ClusterDialog({
  cluster,
  open,
  onOpenChange,
}: {
  cluster?: Cluster
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const optionsQuery = useQuery(dnsZoneOptionsQuery)
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      name: cluster?.name ?? '',
      dns_zone_id: cluster?.dns_zone_id ?? '',
      hostname_prefix: cluster?.hostname_prefix ?? '',
      status: (cluster?.status as Values['status']) ?? 'enabled',
    },
  })
  const mutation = useMutation({
    mutationFn: (values: Values) => saveCluster(values, cluster),
    onSuccess: (response) => {
      toast.success(response.message)
      onOpenChange(false)
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto'>
        <SheetHeader>
          <SheetTitle>{cluster ? '编辑集群' : '创建集群'}</SheetTitle>
          <SheetDescription>
            接入域名由主机前缀和托管域名组合生成。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='cluster-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='name'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>集群名称</FormLabel>
                  <FormControl>
                    <Input placeholder='华东边缘集群' {...field} />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name='dns_zone_id'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>托管域名</FormLabel>
                  <Select value={field.value} onValueChange={field.onChange}>
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue placeholder='选择托管域名' />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {optionsQuery.data?.map((option) => (
                        <SelectItem key={option.id} value={option.id}>
                          {option.domain}
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
              name='hostname_prefix'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>主机前缀</FormLabel>
                  <FormControl>
                    <Input placeholder='edge' {...field} />
                  </FormControl>
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
                  <FormMessage />
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
            form='cluster-form'
            type='submit'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
