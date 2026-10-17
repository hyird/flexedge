import { replaceEqualDeep } from '@tanstack/react-query'
import { expect, test } from 'vitest'
import { ApiBusinessError } from '@/lib/api-response'
import { describeStatus } from '@/lib/status'
import {
  isZoneNotFound,
  recordSyncStatuses,
  zoneRecordRows,
  zoneSyncFilters,
  zoneSyncStatuses,
} from './dns-zone-display'
import { dnsZoneSchema, type DnsRecord, type DnsZone } from './types'

const record = (id: string): DnsRecord => ({
  id,
  type: 'A',
  name: id,
  content: '192.0.2.1',
  ttl: 600,
  proxied: false,
  line_code: 'default',
})

const zone = (patch: Partial<DnsZone> = {}): DnsZone => ({
  id: 'zone',
  revision: 1,
  created_at: '',
  updated_at: '',
  dns_provider_id: 'provider',
  dns_provider: 'aliyun',
  dns_provider_name: 'Aliyun',
  domain: 'example.com',
  sync_status: 'synced',
  desired_revision: 1,
  synced_revision: 1,
  website_count: 0,
  config: { records: [record('custom')] },
  runtime: {
    lines: [],
    projected_records: [record('system')],
    record_states: [
      {
        id: 'custom',
        sync_status: 'failed',
        synced_revision: 0,
        last_error: 'x',
      },
    ],
  },
  ...patch,
})

test('the fixture is a valid zone DTO', () => {
  expect(dnsZoneSchema.parse(zone())).toEqual(zone())
})

test('every displayed zone status has its own label', () => {
  for (const status of ['synced', 'pending', 'queued', 'syncing', 'failed'])
    expect(describeStatus(zoneSyncStatuses, status).label).not.toBe(status)
})

test('sync filters and chips word the same status the same way', () => {
  for (const { id, label } of zoneSyncFilters) {
    if (id === 'issue') expect(label).toBe('同步异常')
    else expect(describeStatus(zoneSyncStatuses, id).label).toBe(label)
  }
  expect(describeStatus(zoneSyncStatuses, 'pending').label).toBe('等待同步')
  for (const id of Object.keys(recordSyncStatuses))
    expect(describeStatus(recordSyncStatuses, id)).toEqual(
      describeStatus(zoneSyncStatuses, id)
    )
})

test('record rows list system records first and attach their sync state', () => {
  const rows = zoneRecordRows(zone())
  expect(rows.map((row) => [row.record.id, row.source])).toEqual([
    ['system', 'system'],
    ['custom', 'custom'],
  ])
  expect(rows[0].state).toBeUndefined()
  expect(rows[1].state).toMatchObject({ sync_status: 'failed' })
})

test('a rejected website CNAME keeps its failed state on the system row', () => {
  const lastError =
    '主机记录 @ 已有A记录，不能再添加CNAME；请删除或修改这些记录，或把该网站域名改为外部解析'
  const rows = zoneRecordRows(
    zone({
      runtime: {
        lines: [],
        projected_records: [{ ...record('website'), type: 'CNAME', name: '@' }],
        record_states: [
          {
            id: 'website',
            sync_status: 'failed',
            synced_revision: 0,
            last_error: lastError,
          },
        ],
      },
    })
  )
  expect(rows[0]).toMatchObject({
    source: 'system',
    state: { sync_status: 'failed', last_error: lastError },
  })
})

test('a live patch of one record keeps every other row’s props by reference', () => {
  // The records table memoises rows on `record` and `state`; the query cache
  // structurally shares a patched snapshot, so only the changed row re-renders.
  const records = Array.from({ length: 300 }, (_, index) => record(`r${index}`))
  const before = zone({ config: { records } })
  const patched = structuredClone(before)
  patched.revision = 2
  patched.config.records[150].content = '192.0.2.150'
  const after = replaceEqualDeep(before, patched)
  const previous = zoneRecordRows(before)
  const next = zoneRecordRows(after)
  const changed = next.filter(
    (row, index) =>
      row.record !== previous[index].record ||
      row.state !== previous[index].state
  )
  expect(changed.map((row) => row.record.id)).toEqual(['r150'])
  expect(after.runtime.lines).toBe(before.runtime.lines)
})

test('only the zone not-found business code means the zone is gone', () => {
  expect(isZoneNotFound(new ApiBusinessError(16201, '托管域名不存在'))).toBe(
    true
  )
  expect(isZoneNotFound(new ApiBusinessError(16205, 'x'))).toBe(false)
})
