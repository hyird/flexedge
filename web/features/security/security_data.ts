import {
  createContext,
  useContext,
  useEffect,
  useMemo,
  useState,
  type SetStateAction,
} from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { useLocation } from '@tanstack/react-router'
import {
  securityWorkspaceQuery,
  securityTelemetryQuery,
  saveSecurityPolicy,
  afterSecurityPolicyWrite,
  saveSharedSecurityList,
  securitySharedListSchema,
  testSecurityRule,
} from './data'
import type { Condition } from './expression_model'
import type { SecurityList } from './lists_model'
import {
  listFromApi,
  policyFromApi,
  policyToApi,
  type SecurityPolicy,
} from './security_mapper'
import { securityPolicySchema } from './transport'

export type { SecurityPolicy } from './security_mapper'
export type RequestSample = Record<Condition['field'], string>
type LandingTab = 'overview' | 'policies' | 'lists' | 'events'
export function useSecuritySource() {
  const client = useQueryClient(),
    core = useQuery(securityWorkspaceQuery)
  const [landingTab, setLandingTab] = useState<LandingTab>('overview')
  const pathname = useLocation({ select: (location) => location.pathname })
  const telemetry = useQuery({
    ...securityTelemetryQuery,
    enabled:
      /^\/security\/?$/.test(pathname) &&
      (landingTab === 'overview' || landingTab === 'events'),
  })
  const [now, setNow] = useState(() => Date.now())
  const lists = useMemo(
    () => core.data?.lists.map(listFromApi) ?? [],
    [core.data?.lists]
  )
  const policies = useMemo(
    () =>
      core.data?.policies.map((policy) =>
        policyFromApi(policy, core.data.catalog)
      ) ?? [],
    [core.data]
  )
  useEffect(() => {
    const expiry = Math.min(
      ...lists.flatMap((list) =>
        list.members
          .map((member) => member.expiresAt ?? Infinity)
          .filter((time) => time > now)
      )
    )
    if (!Number.isFinite(expiry)) return
    const timer = setTimeout(
      () => setNow(Date.now()),
      Math.min(2147483647, Math.max(0, expiry - Date.now()))
    )
    return () => clearTimeout(timer)
  }, [lists, now])
  const setPolicies = async (
    next: SetStateAction<SecurityPolicy[]>,
    expectedRevision?: number
  ) => {
    if (!core.data) throw new Error('安全规则集尚未加载')
    const updates = typeof next === 'function' ? next(policies) : next
    const changed = updates.filter(
      (item) => policies.find((previous) => previous.id === item.id) !== item
    )
    if (changed.length !== 1) throw new Error('每次只能保存一个规则集')
    const input = changed[0],
      previous = core.data.policies.find((item) => item.id === input.id)
    const response = await saveSecurityPolicy(
      policyToApi(input, lists, core.data.catalog, previous),
      previous
        ? { id: previous.id, revision: expectedRevision ?? previous.revision }
        : undefined
    )
    const saved = securityPolicySchema.parse(response.data)
    client.setQueryData(securityWorkspaceQuery.queryKey, {
      ...core.data,
      policies: previous
        ? core.data.policies.map((item) =>
            item.id === saved.id ? saved : item
          )
        : [...core.data.policies, saved],
    })
    void afterSecurityPolicyWrite(client)
    return policyFromApi(saved, core.data.catalog)
  }
  const setLists = async (
    next: SetStateAction<SecurityList[]>,
    expectedRevision?: number
  ) => {
    if (!core.data) throw new Error('安全清单尚未加载')
    const updates = typeof next === 'function' ? next(lists) : next
    const changed = updates.filter(
      (item) => lists.find((previous) => previous.id === item.id) !== item
    )
    if (changed.length !== 1) throw new Error('每次只能保存一个清单')
    const input = changed[0],
      previous = core.data.lists.find((item) => item.id === input.id)
    const response = await saveSharedSecurityList(
      {
        name: input.name,
        description: input.description,
        kind: input.kind,
        members: input.members.map((member) => ({
          id: member.id,
          value: member.value,
          note: member.note,
          source: member.source,
          rule_id: member.ruleId ?? '',
          added_unix_ms: member.addedAt,
          expires_unix_ms: member.expiresAt ?? 0,
        })),
      },
      previous
        ? { id: previous.id, revision: expectedRevision ?? previous.revision }
        : undefined
    )
    const saved = securitySharedListSchema.parse(response.data)
    client.setQueryData(securityWorkspaceQuery.queryKey, {
      ...core.data,
      lists: previous
        ? core.data.lists.map((item) => (item.id === saved.id ? saved : item))
        : [...core.data.lists, saved],
    })
    void afterSecurityPolicyWrite(client)
    return listFromApi(saved)
  }
  const testRule = async (
    policy: string,
    rule: string,
    request: RequestSample,
    count: number,
    website: string
  ) => {
    const response = await testSecurityRule(
      policy,
      rule,
      request,
      count,
      website
    )
    if (!response.data) throw new Error('测试结果为空')
    return response.data
  }
  return {
    policies,
    lists,
    websites:
      core.data?.websites.map((website) => ({
        id: website.id,
        name: website.name,
        policyId: website.policy_id || null,
      })) ?? [],
    events: telemetry.data?.events ?? [],
    hourly: telemetry.data?.hourly ?? [],
    requests: telemetry.data?.requests ?? 0,
    distribution: telemetry.data?.distribution ?? {},
    now,
    landingTab,
    setLandingTab,
    setLists,
    setPolicies,
    testRule,
    core,
    telemetry,
  }
}
export type SecurityData = ReturnType<typeof useSecuritySource>
export const SecurityDataContext = createContext<SecurityData | null>(null)
export function useSecurityData() {
  const data = useContext(SecurityDataContext)
  if (!data) throw new Error('缺少安全数据上下文')
  return data
}
