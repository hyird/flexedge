import { useMemo } from 'react'
import { type Resolver, useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { Certificate, Cluster, PageData } from '@/lib/types'
import { Button } from '@/components/ui/button'
import { Form } from '@/components/ui/form'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { WebsiteBasicTab } from './website-basic-tab'
import { WebsiteDomainsTab } from './website-domains-tab'
import { WebsiteFeaturesTab } from './website-features-tab'
import { WebsiteOriginsTab } from './website-origins-tab'
import { WebsiteRoutesTab } from './website-routes-tab'
import {
  defaultWebsiteConfig,
  parseRouteHeaders,
  routeRuleToForm,
  type WebsiteFormValues,
  websiteFormSchema,
} from './website-form'
import type { Website, WebsiteConfig } from './types'

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
  const queryClient = useQueryClient()
  const config = website?.config ?? defaultWebsiteConfig()
  const certificatesQuery = useQuery({
    queryKey: [...queryKeys.certificates, 'usable-options'],
    queryFn: () =>
      getData<PageData<Certificate>>('/certificates/', {
        usable: true,
      }).then((data) => data.list),
  })
  const form = useForm<WebsiteFormValues>({
    resolver: zodResolver(websiteFormSchema) as Resolver<WebsiteFormValues>,
    defaultValues: {
      cluster_id: website?.cluster_id ?? '',
      status: (website?.status as WebsiteFormValues['status']) ?? 'enabled',
      name: config.name ?? '',
      domains:
        config.domains.length > 0
          ? config.domains
          : [
              {
                id: crypto.randomUUID(),
                hostname: '',
                dns_mode: 'managed' as const,
              },
            ],
      origins:
        config.origins.length > 0
          ? config.origins
          : [
              {
                id: crypto.randomUUID(),
                group: 'default',
                protocol: 'http' as const,
                host: '',
                port: 80,
                role: 'primary' as const,
                weight: 100,
                status: 'enabled' as const,
              },
            ],
      default_origin_group: config.default_origin_group,
      origin_host_header: config.origin_host_header,
      pass_client_ip: config.pass_client_ip,
      health_check_enabled: config.health_check_enabled,
      health_check_path: config.health_check_path,
      health_check_interval_seconds: config.health_check_interval_seconds,
      health_check_timeout_seconds: config.health_check_timeout_seconds,
      health_check_expected_status: config.health_check_expected_status,
      healthy_threshold: config.healthy_threshold,
      unhealthy_threshold: config.unhealthy_threshold,
      access_log_enabled: config.access_log_enabled,
      access_log_request_headers: config.access_log_request_headers,
      access_log_request_body: config.access_log_request_body,
      access_log_response_headers: config.access_log_response_headers,
      access_log_query_params: config.access_log_query_params,
      access_log_cookies: config.access_log_cookies,
      access_log_referer: config.access_log_referer,
      access_log_user_agent: config.access_log_user_agent,
      access_log_status_code_ranges:
        config.access_log_status_code_ranges.filter(
          (range): range is WebsiteFormValues['access_log_status_code_ranges'][number] =>
            ['1xx', '2xx', '3xx', '4xx', '5xx'].includes(range)
        ),
      access_log_client_abort: config.access_log_client_abort,
      https_enabled: config.https_enabled,
      certificate_ids: config.certificate_ids,
      minimum_tls_version: config.minimum_tls_version,
      force_https: config.force_https,
      http2_enabled: config.http2_enabled,
      hsts_enabled: config.hsts_enabled,
      response_compression_enabled: config.response_compression_enabled,
      response_compression_min_bytes: config.response_compression_min_bytes,
      response_compression_max_bytes: config.response_compression_max_bytes,
      response_compression_algorithms:
        config.response_compression_algorithms.filter(
          (
            algorithm
          ): algorithm is WebsiteFormValues['response_compression_algorithms'][number] =>
            ['br', 'zstd', 'gzip'].includes(algorithm)
        ),
      response_compression_mime_types: config.response_compression_mime_types,
      response_compression_extensions: config.response_compression_extensions,
      response_compression_excluded_extensions:
        config.response_compression_excluded_extensions,
      route_rules: config.route_rules.map(routeRuleToForm),
    },
  })
  const domains = useFieldArray({
    control: form.control,
    name: 'domains',
    keyName: 'formKey',
  })
  const origins = useFieldArray({
    control: form.control,
    name: 'origins',
    keyName: 'formKey',
  })
  const routeRules = useFieldArray({
    control: form.control,
    name: 'route_rules',
    keyName: 'formKey',
  })
  const watchedOrigins = form.watch('origins')
  const originGroups = useMemo(() => {
    const groups = new Map<
      string,
      {
        name: string
        indexes: number[]
        enabledCount: number
        enabledPrimaryCount: number
      }
    >()

    watchedOrigins.forEach((origin, index) => {
      const name = origin.group.trim()
      if (!name) return

      const group = groups.get(name) ?? {
        name,
        indexes: [],
        enabledCount: 0,
        enabledPrimaryCount: 0,
      }
      group.indexes.push(index)
      if (origin.status === 'enabled') {
        group.enabledCount += 1
        if (origin.role === 'primary') group.enabledPrimaryCount += 1
      }
      groups.set(name, group)
    })

    return Array.from(groups.values())
  }, [watchedOrigins])
  const selectableOriginGroups = useMemo(
    () => originGroups.filter((group) => group.enabledCount > 0),
    [originGroups]
  )
  const appendOrigin = (
    group: string,
    role: WebsiteFormValues['origins'][number]['role']
  ) => {
    origins.append({
      id: crypto.randomUUID(),
      group,
      protocol: 'http',
      host: '',
      port: 80,
      role,
      weight: 100,
      status: 'enabled',
    })
  }
  const createOriginGroup = () => {
    let suffix = originGroups.length + 1
    let name = `源站组 ${suffix}`
    while (originGroups.some((group) => group.name === name)) {
      suffix += 1
      name = `源站组 ${suffix}`
    }
    appendOrigin(name, 'primary')
  }
  const renameOriginGroup = (currentName: string, draftName: string) => {
    const nextName = draftName.trim()
    const hasControlCharacter = Array.from(nextName).some((character) => {
      const code = character.charCodeAt(0)
      return code < 32 || code === 127
    })
    if (nextName === currentName) return true
    if (!nextName || nextName.length > 100 || hasControlCharacter) {
      toast.error('源站组名称需为 1–100 个非控制字符')
      return false
    }
    if (originGroups.some((group) => group.name === nextName)) {
      toast.error('源站组名称不能重复')
      return false
    }

    form.getValues('origins').forEach((origin, index) => {
      if (origin.group.trim() === currentName) {
        form.setValue(`origins.${index}.group`, nextName, {
          shouldDirty: true,
          shouldValidate: true,
        })
      }
    })
    if (form.getValues('default_origin_group').trim() === currentName) {
      form.setValue('default_origin_group', nextName, {
        shouldDirty: true,
        shouldValidate: true,
      })
    }
    const rules = form.getValues('route_rules')
    const nextRules = rules.map((rule) =>
      rule.origin_group === currentName
        ? { ...rule, origin_group: nextName }
        : rule
    )
    if (nextRules.some((rule, index) => rule !== rules[index])) {
      form.setValue('route_rules', nextRules, {
        shouldDirty: true,
        shouldValidate: true,
      })
    }
    return true
  }
  const isOriginGroupUsedByRoute = (groupName: string) => {
    return form
      .getValues('route_rules')
      .some(
        (rule) => rule.action === 'proxy' && rule.origin_group === groupName
      )
  }
  const removeOrigin = (index: number) => {
    const currentOrigins = form.getValues('origins')
    const origin = currentOrigins[index]
    if (!origin) return

    const groupName = origin.group.trim()
    const removesGroup = !currentOrigins.some(
      (item, itemIndex) =>
        itemIndex !== index && item.group.trim() === groupName
    )
    if (
      removesGroup &&
      form.getValues('default_origin_group').trim() === groupName
    ) {
      toast.error('请先在“基础”页切换默认源站组，再删除该组')
      return
    }
    if (removesGroup && isOriginGroupUsedByRoute(groupName)) {
      toast.error('该源站组仍被代理路由引用，请先修改路由规则')
      return
    }
    origins.remove(index)
  }
  const mutation = useMutation({
    mutationFn: (values: WebsiteFormValues) => {
      const bodyConfig: WebsiteConfig = {
        ...config,
        name: values.name,
        domains: values.domains,
        origins: values.origins,
        default_origin_group: values.default_origin_group,
        origin_host_header: values.origin_host_header,
        pass_client_ip: values.pass_client_ip,
        health_check_enabled: values.health_check_enabled,
        health_check_path: values.health_check_path,
        health_check_interval_seconds: values.health_check_interval_seconds,
        health_check_timeout_seconds: values.health_check_timeout_seconds,
        health_check_expected_status: values.health_check_expected_status,
        healthy_threshold: values.healthy_threshold,
        unhealthy_threshold: values.unhealthy_threshold,
        access_log_enabled: values.access_log_enabled,
        access_log_request_headers: values.access_log_request_headers,
        access_log_request_body: values.access_log_request_body,
        access_log_response_headers: values.access_log_response_headers,
        access_log_query_params: values.access_log_query_params,
        access_log_cookies: values.access_log_cookies,
        access_log_referer: values.access_log_referer,
        access_log_user_agent: values.access_log_user_agent,
        access_log_status_code_ranges: values.access_log_status_code_ranges,
        access_log_client_abort: values.access_log_client_abort,
        https_enabled: values.https_enabled,
        certificate_ids: values.certificate_ids,
        minimum_tls_version: values.minimum_tls_version,
        force_https: values.force_https,
        http2_enabled: values.http2_enabled,
        hsts_enabled: values.hsts_enabled,
        response_compression_enabled: values.response_compression_enabled,
        response_compression_min_bytes: values.response_compression_min_bytes,
        response_compression_max_bytes: values.response_compression_max_bytes,
        response_compression_algorithms: values.response_compression_algorithms,
        response_compression_mime_types: values.response_compression_mime_types,
        response_compression_extensions: values.response_compression_extensions,
        response_compression_excluded_extensions:
          values.response_compression_excluded_extensions,
        route_rules: values.route_rules.map(
          ({ request_headers_text, response_headers_text, ...rule }) => ({
            ...rule,
            request_headers: parseRouteHeaders(request_headers_text),
            response_headers: parseRouteHeaders(response_headers_text),
          })
        ),
      }
      const body = { status: values.status, config: bodyConfig }
      const url = website
        ? `/websites/${website.id}?cluster_id=${values.cluster_id}`
        : `/websites/?cluster_id=${values.cluster_id}`
      return website
        ? sendData('put', url, body, website.revision)
        : sendData('post', url, body)
    },
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: queryKeys.websites })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='w-full overflow-hidden p-0 sm:max-w-4xl'>
        <SheetHeader className='px-6 pt-6'>
          <SheetTitle>{website ? '编辑网站' : '创建网站'}</SheetTitle>
          <SheetDescription>
            配置会通过后台任务安全分发到目标集群。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='website-form'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <Tabs defaultValue='basic' className='gap-0'>
              <div className='overflow-x-auto border-b px-6 pb-3'>
                <TabsList>
                  <TabsTrigger value='basic'>基础</TabsTrigger>
                  <TabsTrigger value='domains'>域名</TabsTrigger>
                  <TabsTrigger value='origins'>源站</TabsTrigger>
                  <TabsTrigger value='features'>功能</TabsTrigger>
                  <TabsTrigger value='routes'>路由规则</TabsTrigger>
                </TabsList>
              </div>
              <ScrollArea className='h-[58svh] px-6'>
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
                <WebsiteFeaturesTab
                  form={form}
                  certificates={certificatesQuery.data ?? []}
                />
                <WebsiteRoutesTab
                  form={form}
                  routeRules={routeRules}
                  selectableOriginGroups={selectableOriginGroups}
                />
                </ScrollArea>
            </Tabs>
          </form>
        </Form>
        <SheetFooter className='px-6 py-4'>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='website-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存并分发'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
