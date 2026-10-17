import { type ReactNode, useEffect, useMemo, useRef, useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Chip, Skeleton, Tabs, useMediaQuery } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { useArrivalState } from '@/lib/history-state'
import { useRevisionDraft } from '@/lib/revision-draft'
import { LinkButton } from '@/components/link-button'
import { Page, QueryNotice } from '@/components/page'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { SaveBar } from '@/components/save-bar'
import {
  TabPanelHoldContext,
  useTabPanelHold,
} from '@/components/tab-panel-hold'
import { usableCertificateOptionsQuery } from '@/features/certificates/data'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import {
  afterWebsiteWrite,
  createWebsite,
  hostedZoneOptionsQuery,
  updateWebsite,
  websiteDetailQuery,
} from './data'
import { collectOriginGroups } from './origin-groups'
import { initialRouteEditorView } from './route-tabs'
import type { Website } from './types'
import { useWebsite, type WebsiteArrival } from './website-context'
import { DomainsSection } from './website-domain-section'
import { validateWebsiteForm, type WebsiteFormValues } from './website-form'
import {
  websiteFormToConfig,
  websiteToFormValues,
} from './website-form-mapping'
import {
  HealthSection,
  OriginSettingsSection,
  OriginsSection,
} from './website-origin-section'
import { WebsiteRouteEditor } from './website-route-editor'
import {
  collectWebsiteFormErrors,
  countWebsiteSectionErrors,
  firstWebsiteErrorTarget,
  websiteSections,
  websiteSectionFrom,
  type WebsiteSectionId,
  type WebsiteSectionProps,
} from './website-sections'
import {
  BasicSection,
  CompressionSection,
  HttpsSection,
  LogsSection,
} from './website-settings-sections'
import { WebsiteCacheSection } from './website_cache_section'
import { WebsiteSecuritySection } from './website_security_section'

const sectionComponents: Record<
  Exclude<WebsiteSectionId, 'routes'>,
  (props: WebsiteSectionProps) => ReactNode
> = {
  cache: WebsiteCacheSection,
  security: WebsiteSecuritySection,
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
  const { id: websiteId } = useWebsite()
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
      <SectionNav section={section} onChange={setSection} errorCounts={{}}>
        <EditorSkeleton />
      </SectionNav>
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
      className='flex flex-col gap-4'
    >
      <Skeleton className='h-56 rounded-2xl' />
      <Skeleton className='h-32 rounded-2xl' />
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
      className='w-full min-w-0 gap-4 lg:items-start'
    >
      {/* HeroUI's ScrollShadow only writes the overflow flags of the current
       * orientation, so crossing the breakpoint would leave a stale
       * horizontal "can scroll" flag showing a chevron on the vertical list;
       * a fresh list per orientation starts clean. */}
      <Tabs.ListContainer
        key={wide ? 'vertical' : 'horizontal'}
        className='min-w-0 lg:sticky lg:top-4 lg:w-max lg:shrink-0'
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
        className='m-0 min-w-0 flex-1 p-0'
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
  const client = useQueryClient()
  const navigate = useNavigate()
  // The user's changes; absent while the draft shows the clean values.
  const [edits, setEdits] = useState<WebsiteFormValues>()
  const [formVersion, setFormVersion] = useState(0)
  const [showErrors, setShowErrors] = useState(false)
  const [routeView, setRouteView] = useState(initialRouteEditorView)
  const [pendingFocus, setPendingFocus] = useState<WebsiteSectionId | null>(
    null
  )
  const contentRef = useRef<HTMLDivElement>(null)
  const draft = useRevisionDraft({
    server: website,
    save: async ({ data }: SaveInput, revision) => {
      const input = {
        cluster_id: data.cluster_id,
        status: data.status,
        config: websiteFormToConfig(data),
      }
      if (!website || revision === undefined) {
        const created = await createWebsite(input)
        return { created, revision: created.revision }
      }
      const result = await updateWebsite(input, { id: website.id, revision })
      return { created: null, revision: result.revision }
    },
    onSaved: ({ created }) => {
      void afterWebsiteWrite(client, website?.id ?? created?.id)
      if (created) {
        toastWriteSuccess('网站已创建，配置正在分发到边缘节点', {
          resourceType: 'website',
          resourceId: created.id,
        })
        void navigate({
          to: '/websites',
          state: { website: { id: created.id } },
          ignoreBlocker: true,
        })
        return
      }
      if (!website) return
      toastWriteSuccess('配置已保存，正在分发到边缘节点', {
        resourceType: 'website',
        resourceId: website.id,
      })
      setEdits(undefined)
      setShowErrors(false)
    },
    load: () => {
      setEdits(undefined)
      setFormVersion((version) => version + 1)
      setShowErrors(false)
    },
    reload: onReload,
  })
  const base = draft.base
  // Clean values: the last save until the server reaches it, else `base`'s.
  const savedDraft = draft.savedValues?.draft
  const clean = useMemo(
    () => savedDraft ?? websiteToFormValues(base),
    [savedDraft, base]
  )
  const values = edits ?? clean
  const dirty =
    edits !== undefined && JSON.stringify(edits) !== JSON.stringify(clean)

  // Managed domains are validated against their hosted zone once the list
  // is known; until then only the hostname format is checked here.
  const hostedZones = useQuery(
    hostedZoneOptionsQuery(
      values.domains.some((domain) => domain.dns_mode === 'managed')
    )
  ).data
  // Names of selectable and already bound certificates. The editor reads the
  // options itself whenever HTTPS is on, so validation never depends on the
  // HTTPS section having been opened (the section shares this read).
  const loadedCertificates = useQuery({
    ...usableCertificateOptionsQuery,
    enabled: values.https_enabled,
    meta: { inlineError: true },
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

  function replace(change: (current: WebsiteFormValues) => WebsiteFormValues) {
    const next = change(edits ?? clean)
    if (JSON.stringify(next) === JSON.stringify(clean)) {
      setEdits(undefined)
      draft.track(false)
      return
    }
    setEdits(next)
    draft.track(true)
  }

  function update<K extends keyof WebsiteFormValues>(
    key: K,
    value: WebsiteFormValues[K]
  ) {
    replace((current) => ({ ...current, [key]: value }))
  }

  function save() {
    const parsed = validateWebsiteForm(values, hostedZones, knownCertificates)
    if (!parsed.success) {
      const found = collectWebsiteFormErrors(parsed.error.issues)
      const target = firstWebsiteErrorTarget(found)
      setShowErrors(true)
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
    draft.save({ data: parsed.data, draft: values })
  }

  const sectionProps: WebsiteSectionProps = {
    values,
    errors,
    update,
    replace,
    website: base,
  }
  const SectionContent =
    section === 'routes' ? undefined : sectionComponents[section]
  const groups = collectOriginGroups(values.origins).map((group) => group.name)

  return (
    <>
      <RevisionConflictAlert draft={draft} />
      <SectionNav
        section={section}
        onChange={onSectionChange}
        errorCounts={errorCounts}
      >
        <div
          ref={contentRef}
          // A load or a clean draft following the server remounts the
          // fields; the first change keeps `base`, so editing never does.
          key={`${formVersion}:${base?.revision ?? 'new'}`}
          inert={draft.isSaving}
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
        isSaving={draft.isSaving}
        onSave={save}
        onDiscard={draft.discard}
        saveLabel={base ? '保存并分发' : '创建网站'}
        message={base ? '有未保存的更改' : '新网站尚未创建'}
        error={
          Object.keys(errors).length
            ? `${Object.keys(errors).length} 处配置需要修改`
            : draft.error
              ? apiErrorMessage(draft.error)
              : undefined
        }
      />
    </>
  )
}
