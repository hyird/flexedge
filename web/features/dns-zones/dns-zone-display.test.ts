import { expect, test } from 'vitest'
import { ApiBusinessError } from '@/lib/api-response'
import {
  displaySyncStatus,
  isZoneNotFound,
  zoneRecordRows,
} from './dns-zone-display'
import type { DnsRecord, DnsZone } from './types'

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
  website_count: 0,
  config: { records: [record('custom')] },
  runtime: {
    records_imported: true,
    lines: [],
    projected_records: [record('system')],
    record_states: [
      {
        id: 'custom',
        sync_status: 'failed',
        last_error: 'x',
      },
    ],
    conflicts: [],
  },
  ...patch,
})

test('a conflict status without remaining conflicts displays as consistent', () => {
  expect(displaySyncStatus(zone({ sync_status: 'conflict' }))).toBe(
    'consistent'
  )
})

test('record rows list system records first and attach their sync state', () => {
  const rows = zoneRecordRows(zone())
  expect(rows.map((row) => [row.id, row.source])).toEqual([
    ['system', 'system'],
    ['custom', 'custom'],
  ])
  expect(rows[0].state).toBeUndefined()
  expect(rows[1].state).toMatchObject({ sync_status: 'failed' })
})

test('a rejected website CNAME keeps its failed state on the system row', () => {
  const lastError =
    '主机记录 @ 已有 A 记录，不能再添加 CNAME；请删除或修改这些记录，或把该网站域名改为外部解析'
  const rows = zoneRecordRows(
    zone({
      runtime: {
        records_imported: true,
        lines: [],
        projected_records: [{ ...record('website'), type: 'CNAME', name: '@' }],
        record_states: [
          { id: 'website', sync_status: 'failed', last_error: lastError },
        ],
        conflicts: [],
      },
    })
  )
  expect(rows[0]).toMatchObject({
    source: 'system',
    state: { sync_status: 'failed', last_error: lastError },
  })
})

test('only the zone not-found business code means the zone is gone', () => {
  expect(isZoneNotFound(new ApiBusinessError(16201, '托管域名不存在'))).toBe(
    true
  )
  expect(isZoneNotFound(new ApiBusinessError(16205, 'x'))).toBe(false)
})
