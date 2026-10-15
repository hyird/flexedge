import { type ReactNode, useEffect, useRef, useState } from 'react'
import { useMutation, useQuery } from '@tanstack/react-query'
import { getRouteApi, useNavigate } from '@tanstack/react-router'
import {
  Alert,
  Button,
  Chip,
  Skeleton,
  Tabs,
  useMediaQuery,
} from '@heroui/react'
import { apiErrorMessage, isRevisionConflict } from '@/lib/api'
import { useArrivalState } from '@/lib/history-state'
import { LinkButton } from '@/components/link-button'
import { Page, QueryNotice } from '@/components/page'
import { SaveBar } from '@/components/save-bar'
import {
  TabPanelHoldContext,
  useTabPanelHold,
} from '@/components/tab-panel-hold'
import { usableCertificateOptionsQuery } from '@/features/certificates/data'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import {
  createWebsite,
  hostedZoneOptionsQuery,
  updateWebsite,
  websiteDetailQuery,
} from './data'
import { collectOriginGroups } from './origin-groups'
import { initialRouteEditorView } from './route-tabs'
import type { Website } from './types'
import type { WebsiteArrival } from './website-context'
import { validateWebsiteForm, type WebsiteFormValues } from './website-form'
import {
  websiteFormToConfig,
  websiteToFormValues,
} from './website-form-mapping'
import { WebsiteRouteEditor } from './website-route-editor'
import {
  collectWebsiteFormErrors,
  countWebsiteSectionErrors,
  firstWebsiteErrorTarget,
  websiteSections,
  websiteSectionFrom,
  type WebsiteSectionId,
} from './website-sections'
import {
  BasicSection,
  CompressionSection,
  DomainsSection,
  HealthSection,
  HttpsSection,
  LogsSection,
  OriginSettingsSection,
  OriginsSection,
  type WebsiteSectionProps,
} from './website-settings-sections'

const settingsRoute = getRouteApi('/_authenticated/websites/$websiteId')

const sectionComponents: Record<
  Exclude<WebsiteSectionId, 'routes'>,
  (props: WebsiteSectionProps) => ReactNode
> = {
  basic: BasicSection,
  domains: DomainsSection,
  https: HttpsSection,
  origins: OriginsSection,
  'origin-settings': OriginSettingsSection,
  health: HealthSection,
  compression: CompressionSection,
  logs: LogsSection,
}

/** `/websites/$websiteId` settings tab. The group is component state; the
 * arrival may request one (`state.website.section`). */
export function WebsiteSettingsPage({ initial }: { initial?: WebsiteArrival }) {
  const { websiteId } = settingsRoute.useParams()
  const [section, setSection] = useState(() =>
    websiteSectionFrom(initial?.section)
  )
  const query = useQuery(websiteDetailQuery(websiteId))
  if (!query.data)
    return query.isError ? (
      <QueryNotice
        query={query}
        title='网站配置加载失败'
        onRetry={() => void query.refetch()}
      />
    ) : (
      <EditorSkeleton />
    )
  return (
    <WebsiteEditor
      key={websiteId}
      website={query.data}
      section={section}
      onSectionChange={setSection}
      onReload={async () => (await query.refetch()).data}
    />
  )
}

/** `/websites/new`. The group is component state seeded from the arrival
 * (`state.website.section`). */
export function WebsiteCreatePage() {
  const [section, setSection] = useArrivalState((state) =>
    websiteSectionFrom(state.website?.section)
  )
  return (
    <Page
      title='创建网站'
      description='填写域名和源站后创建，配置会分发到所属集群的全部边缘节点。'
      actions={<LinkButton href='/websites'>返回列表</LinkButton>}
    >
      <WebsiteEditor section={section} onSectionChange={setSection} />
    </Page>
  )
}

function EditorSkeleton() {
  return (
    <div
      aria-busy
      aria-label='正在加载网站配置'
      className='grid gap-6 lg:grid-cols-[11rem_minmax(0,1fr)]'
    >
      <div className='flex gap-2 lg:flex-col'>
        {websiteSections.map((section) => (
          <Skeleton
            key={section.id}
            className='h-8 w-20 rounded-lg lg:w-full'
          />
        ))}
      </div>
      <div className='flex flex-col gap-4'>
        <Skeleton className='h-56 rounded-2xl' />
        <Skeleton className='h-32 rounded-2xl' />
      </div>
    </div>
  )
}

function SectionNav({
  section,
  onChange,
  errorCounts,
  children,
}: {
  section: WebsiteSectionId
  onChange: (section: WebsiteSectionId) => void
  errorCounts: Partial<Record<WebsiteSectionId, number>>
  children: ReactNode
}) {
  const wide = useMediaQuery('(min-width: 1024px)')
  // A shorter group keeps the page's scroll position (see `useTabPanelHold`).
  const {
    ref: panelRef,
    style: panelStyle,
    holdForSwitch,
    hold,
  } = useTabPanelHold()
  return (
    <Tabs
      selectedKey={section}
      onSelectionChange={(key) => {
        holdForSwitch()
        onChange(key as WebsiteSectionId)
      }}
      variant='secondary'
      orientation={wide ? 'vertical' : 'horizontal'}
      align='start'
      className='w-full min-w-0 gap-6 lg:items-start'
    >
      {/* HeroUI's ScrollShadow only writes the overflow flags of the current
       * orientation, so crossing the breakpoint would leave a stale
       * horizontal "can scroll" flag showing a chevron on the vertical list;
       * a fresh list per orientation starts clean. */}
      <Tabs.ListContainer
        key={wide ? 'vertical' : 'horizontal'}
        className='min-w-0 lg:sticky lg:top-4 lg:w-44 lg:shrink-0'
      >
        <Tabs.List aria-label='网站配置分组' className='lg:w-full'>
          {websiteSections.map((item) => {
            const count = errorCounts[item.id]
            return (
              <Tabs.Tab
                key={item.id}
                id={item.id}
                className='w-auto flex-none px-3 lg:w-full'
              >
                <span className='flex w-full items-center gap-2'>
                  {item.label}
                  {count ? (
                    <Chip
                      size='sm'
                      variant='soft'
                      color='danger'
                      className='ml-auto tabular-nums'
                      aria-label={`${count} 处错误`}
                    >
                      {count}
                    </Chip>
                  ) : null}
                </span>
                <Tabs.Indicator />
              </Tabs.Tab>
            )
          })}
        </Tabs.List>
      </Tabs.ListContainer>
      <Tabs.Panel
        id={section}
        ref={panelRef}
        style={panelStyle}
        className='min-w-0 flex-1'
      >
        <TabPanelHoldContext value={hold}>{children}</TabPanelHoldContext>
      </Tabs.Panel>
    </Tabs>
  )
}

type SaveInput = { data: WebsiteFormValues; draft: WebsiteFormValues }

/** Draft editor shared by `/websites/new` (no `website`) and the detail
 * settings tab. The draft survives group switches; the save bar guards
 * navigation and, inside the detail `PageTabs`, switching tabs while it
 * differs from the loaded configuration. */
function WebsiteEditor({
  website,
  section,
  onSectionChange,
  onReload,
}: {
  website?: Website
  section: WebsiteSectionId
  onSectionChange: (section: WebsiteSectionId) => void
  /** Re-reads the website after a revision conflict. */
  onReload?: () => Promise<Website | undefined>
}) {
  const navigate = useNavigate()
  // `base` is the revision the draft was loaded from; If-Match uses it, not
  // the live detail stream, so concurrent edits surface as conflicts.
  const [base, setBase] = useState(website)
  const [initial, setInitial] = useState(() => websiteToFormValues(website))
  const [values, setValues] = useState(initial)
  const [formVersion, setFormVersion] = useState(0)
  const [showErrors, setShowErrors] = useState(false)
  const [saveError, setSaveError] = useState('')
  const [conflict, setConflict] = useState(false)
  const [savedFrom, setSavedFrom] = useState<number | null>(null)
  const [routeView, setRouteView] = useState(initialRouteEditorView)
  const [pendingFocus, setPendingFocus] = useState<WebsiteSectionId | null>(
    null
  )
  const contentRef = useRef<HTMLDivElement>(null)
  const dirty = JSON.stringify(values) !== JSON.stringify(initial)

  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: async ({ data }: SaveInput) => {
      const input = {
        cluster_id: data.cluster_id,
        status: data.status,
        config: websiteFormToConfig(data),
      }
      if (!base) {
        const created = await createWebsite(input)
        return { created, data: created }
      }
      return { created: null, data: (await updateWebsite(input, base)).data }
    },
    onSuccess: ({ created, data: response }, { draft }) => {
      if (!base) {
        toastWriteSuccess('网站已创建，配置正在分发到边缘节点', response)
        if (created)
          void navigate({
            to: '/websites/$websiteId',
            params: { websiteId: created.id },
            ignoreBlocker: true,
          })
        return
      }
      toastWriteSuccess('配置已保存，正在分发到边缘节点', response)
      setInitial(draft)
      setSavedFrom(base.revision)
      setShowErrors(false)
    },
    onError: (error) => {
      setConflict(isRevisionConflict(error))
      setSaveError(apiErrorMessage(error))
    },
  })

  function adopt(next: Website, keepDraft = false) {
    const loaded = websiteToFormValues(next)
    setBase(next)
    setInitial(loaded)
    setSavedFrom(null)
    if (keepDraft) return
    setValues(loaded)
    setFormVersion((version) => version + 1)
    setShowErrors(false)
    setSaveError('')
    setConflict(false)
  }

  // Follow the live detail stream: our own save, or a remote change while the
  // draft is clean, rebases silently; a remote change under a dirty draft is
  // reported instead (and saving would fail with a revision conflict).
  if (website && base && website.revision !== base.revision) {
    if (savedFrom === base.revision) adopt(website, dirty)
    else if (!dirty && !mutation.isPending) adopt(website)
  }
  const remoteChanged =
    !!website && !!base && website.revision !== base.revision

  // Managed domains are validated against their hosted zone once the list
  // is known; until then only the hostname format is checked here.
  const hostedZones = useQuery(
    hostedZoneOptionsQuery(
      values.domains.some((domain) => domain.dns_mode === 'managed')
    )
  ).data
  // Names of selectable and already bound certificates: the HTTPS section
  // loads the options snapshot; this only reads it from the cache.
  const loadedCertificates = useQuery({
    ...usableCertificateOptionsQuery,
    enabled: false,
  }).data
  const knownCertificates = [
    ...(loadedCertificates ?? []),
    ...(base?.certificates ?? []),
  ]

  const errors = showErrors
    ? (() => {
        const result = validateWebsiteForm(
          values,
          hostedZones,
          knownCertificates
        )
        return result.success
          ? {}
          : collectWebsiteFormErrors(result.error.issues)
      })()
    : {}
  const errorCounts = countWebsiteSectionErrors(errors)

  useEffect(() => {
    if (!pendingFocus || pendingFocus !== section) return
    const frame = requestAnimationFrame(() => {
      setPendingFocus(null)
      const invalid = contentRef.current?.querySelector<HTMLElement>(
        '[data-invalid="true"], [data-field-error]'
      )
      if (!invalid) return
      const target = invalid.hasAttribute('data-field-error')
        ? invalid
        : (invalid.querySelector<HTMLElement>('input, textarea, button') ??
          invalid)
      target.scrollIntoView({ block: 'center' })
      target.focus({ preventScroll: true })
    })
    return () => cancelAnimationFrame(frame)
  }, [pendingFocus, section])

  function update<K extends keyof WebsiteFormValues>(
    key: K,
    value: WebsiteFormValues[K]
  ) {
    setValues((current) => ({ ...current, [key]: value }))
  }

  function save() {
    const parsed = validateWebsiteForm(values, hostedZones, knownCertificates)
    if (!parsed.success) {
      const found = collectWebsiteFormErrors(parsed.error.issues)
      const target = firstWebsiteErrorTarget(found)
      setShowErrors(true)
      setSaveError('')
      if (!target) return
      if (target.routeRuleIndex !== undefined) {
        const rule = values.route_rules[target.routeRuleIndex]
        if (rule)
          setRouteView((view) => ({
            ...view,
            phase: rule.action,
            selectedId: rule.id,
          }))
      }
      setPendingFocus(target.section)
      if (target.section !== section) onSectionChange(target.section)
      return
    }
    setSaveError('')
    setConflict(false)
    mutation.mutate({ data: parsed.data, draft: values })
  }

  function discard() {
    setValues(initial)
    setFormVersion((version) => version + 1)
    setShowErrors(false)
    setSaveError('')
    setConflict(false)
  }

  const reload = useMutation({
    mutationFn: async () => (await onReload?.()) ?? website,
    onSuccess: (latest) => {
      if (latest) adopt(latest)
    },
  })

  const sectionProps: WebsiteSectionProps = {
    values,
    errors,
    update,
    replace: setValues,
    website: base,
  }
  const SectionContent =
    section === 'routes' ? undefined : sectionComponents[section]
  const groups = collectOriginGroups(values.origins).map((group) => group.name)

  return (
    <>
      {(conflict || remoteChanged) && (
        <Alert status={conflict ? 'danger' : 'warning'}>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>
              {conflict ? '数据已被其他人更新' : '此网站已在其他地方更新'}
            </Alert.Title>
            <Alert.Description>
              此配置已被其他人更新，重新加载会载入最新配置并丢弃本页未保存的更改。
            </Alert.Description>
          </Alert.Content>
          <Button
            size='sm'
            variant='tertiary'
            className='shrink-0 self-center'
            isPending={reload.isPending}
            onPress={() => reload.mutate()}
          >
            重新加载
          </Button>
        </Alert>
      )}
      <SectionNav
        section={section}
        onChange={onSectionChange}
        errorCounts={errorCounts}
      >
        <div
          ref={contentRef}
          key={formVersion}
          inert={mutation.isPending}
          className='flex min-w-0 flex-col gap-4'
        >
          {SectionContent ? (
            <SectionContent {...sectionProps} />
          ) : (
            <WebsiteRouteEditor
              rules={values.route_rules}
              onChange={(rules) => update('route_rules', rules)}
              groups={groups}
              defaultGroup={values.default_origin_group}
              errors={errors}
              view={routeView}
              onViewChange={setRouteView}
            />
          )}
        </div>
      </SectionNav>
      <SaveBar
        isDirty={dirty}
        isSaving={mutation.isPending}
        onSave={save}
        onDiscard={discard}
        saveLabel={base ? '保存并分发' : '创建网站'}
        message={base ? '有未保存的更改' : '新网站尚未创建'}
        error={
          Object.keys(errors).length
            ? `${Object.keys(errors).length} 处配置需要修改`
            : saveError
        }
      />
    </>
  )
}
