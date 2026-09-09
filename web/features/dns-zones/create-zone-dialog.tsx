import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery } from '@tanstack/react-query'
import { toast } from 'sonner'
import { apiErrorMessage } from '@/lib/api'
import { Button } from '@/components/ui/button'
import {
  Form,
  FormControl,
  FormDescription,
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
import type { DnsProvider } from '@/features/providers/types'
import { availableDnsZonesQuery, createDnsZone } from './data'

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
  const form = useForm<CreateValues>({
    resolver: zodResolver(createSchema),
    defaultValues: { dns_provider_id: '', domain: '' },
  })
  const providerId = form.watch('dns_provider_id')
  const domain = form.watch('domain')
  const availableQuery = useQuery(availableDnsZonesQuery(providerId))
  const availableZones = availableQuery.data ?? []
  const canSelectDomain =
    !!providerId && availableQuery.isSuccess && availableZones.length > 0
  const canSubmit =
    canSelectDomain && availableZones.some((zone) => zone.domain === domain)
  const mutation = useMutation({
    mutationFn: createDnsZone,
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      form.reset()
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
                    disabled={!canSelectDomain || mutation.isPending}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue
                          placeholder={
                            availableQuery.isLoading
                              ? '正在加载…'
                              : !providerId
                                ? '请先选择 DNS 服务商账号'
                                : availableQuery.isError
                                  ? '域名加载失败'
                                  : availableZones.length === 0
                                    ? '暂无可用域名'
                                    : '选择可用域名'
                          }
                        />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {availableZones.map((zone) => (
                        <SelectItem key={zone.domain} value={zone.domain}>
                          {zone.domain}
                        </SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                  {providerId && availableQuery.isError && (
                    <div role='alert' className='space-y-2'>
                      <p className='text-sm text-destructive'>
                        {apiErrorMessage(availableQuery.error)}
                      </p>
                      <Button
                        type='button'
                        variant='outline'
                        size='sm'
                        disabled={availableQuery.isFetching}
                        onClick={() => void availableQuery.refetch()}
                      >
                        {availableQuery.isFetching
                          ? '正在重试…'
                          : '重新加载域名'}
                      </Button>
                    </div>
                  )}
                  {providerId &&
                    availableQuery.isSuccess &&
                    availableZones.length === 0 && (
                      <div className='space-y-2'>
                        <FormDescription role='status'>
                          此账号暂无可添加的域名，请确认服务商中已有域名且尚未托管。
                        </FormDescription>
                        <Button
                          type='button'
                          variant='outline'
                          size='sm'
                          disabled={availableQuery.isFetching}
                          onClick={() => void availableQuery.refetch()}
                        >
                          {availableQuery.isFetching
                            ? '正在刷新…'
                            : '刷新可用域名'}
                        </Button>
                      </div>
                    )}
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
            disabled={mutation.isPending || !canSubmit}
          >
            {mutation.isPending ? '正在添加…' : '添加并同步'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
