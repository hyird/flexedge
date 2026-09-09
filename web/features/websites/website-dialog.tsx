import { useState } from 'react'
import { type Resolver, useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery } from '@tanstack/react-query'
import { toast } from 'sonner'
import { Button } from '@/components/ui/button'
import { Form } from '@/components/ui/form'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Sheet,
  SheetContent,
  SheetHeader,
  SheetTitle,
  SheetDescription,
} from '@/components/ui/sheet'
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { usableCertificateOptionsQuery } from '@/features/certificates/data'
import type { Cluster } from '@/features/clusters/types'
import { saveWebsite } from './data'
import { routeTabs } from './route-tabs'
import type { Website } from './types'
import { useOriginGroups } from './use-origin-groups'
import { WebsiteBasicTab } from './website-basic-tab'
import { WebsiteDomainsTab } from './website-domains-tab'
import { WebsiteFeaturesTab } from './website-features-tab'
import { type WebsiteFormValues, websiteFormSchema } from './website-form'
import {
  websiteToFormValues,
  websiteFormToConfig,
} from './website-form-mapping'
import { websiteErrorTarget } from './website-form-navigation'
import { WebsiteOriginsTab } from './website-origins-tab'
import { WebsiteRoutesTab } from './website-routes-tab'

export function WebsiteDialog({
  website,
  clusters,
  open,
  onOpenChange,
}: {
  website?: Website
  clusters: Cluster[]
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const [section, setSection] = useState('basic')
  const [invalidRouteIndex, setInvalidRouteIndex] = useState<number | null>(
    null
  )
  const [confirmLeave, setConfirmLeave] = useState(false)
  const certificatesQuery = useQuery(usableCertificateOptionsQuery)
  const form = useForm<WebsiteFormValues>({
    resolver: zodResolver(websiteFormSchema) as Resolver<WebsiteFormValues>,
    defaultValues: websiteToFormValues(website),
  })
  const domains = useFieldArray({
    control: form.control,
    name: 'domains',
    keyName: 'formKey',
  })
  const routeRules = useFieldArray({
    control: form.control,
    name: 'route_rules',
    keyName: 'formKey',
  })
  const {
    origins,
    originGroups,
    selectableOriginGroups,
    appendOrigin,
    createOriginGroup,
    renameOriginGroup,
    removeOrigin,
  } = useOriginGroups(form)
  const mutation = useMutation({
    mutationFn: (values: WebsiteFormValues) =>
      saveWebsite(
        {
          cluster_id: values.cluster_id,
          status: values.status,
          config: websiteFormToConfig(values),
        },
        website
      ),
    onSuccess: () => {
      onOpenChange(false)
      toast.success('配置已保存，正在同步')
    },
  })

  return (
    <Sheet
      open={open}
      onOpenChange={(next) => {
        if (mutation.isPending) return
        if (!next && form.formState.isDirty) setConfirmLeave(true)
        else onOpenChange(next)
      }}
    >
      <SheetContent className='w-full overflow-hidden p-0 sm:max-w-6xl'>
        <SheetHeader className='shrink-0 px-6 pt-6'>
          <SheetTitle>
            {website ? `${website.config.name} · 网站设置` : '创建网站'}
          </SheetTitle>
          <SheetDescription>
            按分类配置网站；保存时统一校验并分发到目标集群。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='website-form'
            className='flex min-h-0 flex-1 flex-col'
            onSubmit={form.handleSubmit(
              (values) => mutation.mutate(values),
              (errors) => {
                const target = websiteErrorTarget(
                  errors,
                  form.getValues('route_rules')
                )
                setInvalidRouteIndex(target.routeIndex)
                setSection(target.section)
                toast.error('请检查表单中的错误字段')
              }
            )}
          >
            <Tabs
              value={section}
              onValueChange={setSection}
              className='min-h-0 flex-1 gap-4 px-4 md:flex-row'
            >
              <div className='shrink-0 overflow-x-auto md:w-28 md:overflow-y-auto'>
                <TabsList className='h-auto gap-1 md:w-full md:flex-col md:items-stretch'>
                  <TabsTrigger value='basic'>基础</TabsTrigger>
                  <TabsTrigger value='domains'>域名</TabsTrigger>
                  <TabsTrigger value='origins'>源站</TabsTrigger>
                  <TabsTrigger value='origin-settings'>回源设置</TabsTrigger>
                  <TabsTrigger value='health'>健康检查</TabsTrigger>
                  <TabsTrigger value='https'>HTTPS</TabsTrigger>
                  <TabsTrigger value='logs'>访问日志</TabsTrigger>
                  <TabsTrigger value='compression'>响应压缩</TabsTrigger>
                  {Object.entries(routeTabs).map(([phase, category]) => (
                    <TabsTrigger key={phase} value={phase}>
                      {category.title}
                    </TabsTrigger>
                  ))}
                </TabsList>
              </div>
              <ScrollArea
                className='min-h-0 min-w-0 flex-1 overflow-hidden rounded-lg border px-4'
                // Size the compression editor against the viewport, without depending on Radix's internal content wrapper.
                viewportClassName={
                  section === 'compression'
                    ? '[container-type:size]'
                    : undefined
                }
              >
                <WebsiteBasicTab
                  form={form}
                  clusters={clusters}
                  selectableOriginGroups={selectableOriginGroups}
                />
                <WebsiteDomainsTab form={form} domains={domains} />
                <WebsiteOriginsTab
                  form={form}
                  origins={origins}
                  originGroups={originGroups}
                  createOriginGroup={createOriginGroup}
                  appendOrigin={appendOrigin}
                  renameOriginGroup={renameOriginGroup}
                  removeOrigin={removeOrigin}
                />
                {(
                  [
                    'origin-settings',
                    'health',
                    'https',
                    'logs',
                    'compression',
                  ] as const
                ).map((category) => (
                  <WebsiteFeaturesTab
                    key={category}
                    category={category}
                    form={form}
                    certificates={certificatesQuery.data ?? []}
                  />
                ))}
                {(['redirect', 'rewrite', 'proxy'] as const).map((phase) => (
                  <WebsiteRoutesTab
                    key={phase}
                    phase={phase}
                    invalidRouteIndex={invalidRouteIndex}
                    clearInvalidRoute={() => setInvalidRouteIndex(null)}
                    form={form}
                    routeRules={routeRules}
                    selectableOriginGroups={selectableOriginGroups}
                  />
                ))}
              </ScrollArea>
            </Tabs>
          </form>
        </Form>
        <div className='flex shrink-0 justify-end gap-2 border-t bg-background px-6 py-4'>
          <Button
            variant='outline'
            disabled={mutation.isPending}
            onClick={() =>
              form.formState.isDirty
                ? setConfirmLeave(true)
                : onOpenChange(false)
            }
          >
            取消
          </Button>
          <Button
            type='submit'
            form='website-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存并分发'}
          </Button>
        </div>
        <ConfirmDialog
          open={confirmLeave}
          onOpenChange={setConfirmLeave}
          title='放弃未保存的配置？'
          desc='当前修改尚未保存和分发，关闭抽屉会丢失这些修改。'
          confirmText='放弃并关闭'
          handleConfirm={() => onOpenChange(false)}
        />
      </SheetContent>
    </Sheet>
  )
}
