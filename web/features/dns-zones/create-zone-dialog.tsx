import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { DnsProvider } from '@/lib/types'
import { Button } from '@/components/ui/button'
import {
  Form,
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
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

const createSchema = z.object({
  dns_provider_id: z.string().uuid('请选择 DNS 服务商'),
  domain: z.string().trim().min(1, '请选择域名'),
})

type CreateValues = z.infer<typeof createSchema>

export function CreateZoneDialog({
  open,
  onOpenChange,
  providers,
}: {
  open: boolean
  onOpenChange: (open: boolean) => void
  providers: DnsProvider[]
}) {
  const queryClient = useQueryClient()
  const form = useForm<CreateValues>({
    resolver: zodResolver(createSchema),
    defaultValues: { dns_provider_id: '', domain: '' },
  })
  const providerId = form.watch('dns_provider_id')
  const availableQuery = useQuery({
    queryKey: [...queryKeys.dnsZones, 'available', providerId],
    queryFn: () =>
      getData<{ list: Array<{ domain: string; status: string }> }>(
        '/dns-zones/available',
        { dns_provider_id: providerId }
      ).then((data) => data.list),
    enabled: !!providerId,
  })
  const mutation = useMutation({
    mutationFn: (values: CreateValues) =>
      sendData('post', '/dns-zones/', values),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      form.reset()
      await queryClient.invalidateQueries({ queryKey: queryKeys.dnsZones })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto'>
        <SheetHeader>
          <SheetTitle>添加托管域名</SheetTitle>
          <SheetDescription>
            从 DNS 服务商账号中选择一个可用区域。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='create-zone-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='dns_provider_id'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>DNS 服务商账号</FormLabel>
                  <Select
                    value={field.value}
                    onValueChange={(value) => {
                      field.onChange(value)
                      form.setValue('domain', '')
                    }}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue placeholder='选择账号' />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {providers.map((provider) => (
                        <SelectItem key={provider.id} value={provider.id}>
                          {provider.name}
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
              name='domain'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>域名</FormLabel>
                  <Select
                    value={field.value}
                    onValueChange={field.onChange}
                    disabled={!providerId || availableQuery.isLoading}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue
                          placeholder={
                            availableQuery.isLoading
                              ? '正在加载…'
                              : '选择可用域名'
                          }
                        />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {availableQuery.data?.map((zone) => (
                        <SelectItem key={zone.domain} value={zone.domain}>
                          {zone.domain}
                        </SelectItem>
                      ))}
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
            type='submit'
            form='create-zone-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在添加…' : '添加并同步'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
