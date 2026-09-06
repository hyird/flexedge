import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type {
  Certificate,
  CertificateProvider,
  DnsZoneOption,
} from '@/lib/types'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
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

export function CertificateDialog({
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
    queryKey: [...queryKeys.providers, 'certificate'],
    queryFn: () => getData<CertificateProvider[]>('/providers/certificate'),
    enabled: !certificate,
  })
  const zonesQuery = useQuery({
    queryKey: [...queryKeys.dnsZones, 'options'],
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
      await queryClient.invalidateQueries({ queryKey: queryKeys.certificates })
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
