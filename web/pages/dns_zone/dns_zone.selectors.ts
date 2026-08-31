import type { DnsLineItem, DnsRecordItem, DnsRecordQuery, DnsZoneItem } from './dns_zone.types';
import type { PaginatedResult } from '@/utils/pagination.types';

function recordView(
    dnsZone: DnsZoneItem,
    record: DnsZoneItem['config']['records'][number],
    managed: boolean
): DnsRecordItem {
    const state = dnsZone.runtime.record_states.find((item) => item.id === record.id);
    const line = dnsZone.runtime.lines.find((item) => item.code === record.line_code);
    return {
        ...record,
        line_name: line?.display_name || line?.name || `未知线路（${record.line_code}）`,
        managed,
        sync_status: state?.sync_status ?? 'pending',
        last_error: state?.last_error,
    };
}

export function selectDnsRecords(
    dnsZone: DnsZoneItem,
    query?: DnsRecordQuery
): PaginatedResult<DnsRecordItem> {
    const keyword = query?.keyword?.trim().toLowerCase();
    const all = [
        ...dnsZone.runtime.projected_records.map((record) => recordView(dnsZone, record, true)),
        ...dnsZone.config.records.map((record) => recordView(dnsZone, record, false)),
    ].filter((record) => {
        if (!keyword) return true;
        return [record.type, record.name, record.content, record.line_name].some((value) =>
            value.toLowerCase().includes(keyword)
        );
    });
    const page = Number(query?.page ?? 1);
    const pageSize = Number(query?.pageSize ?? 20);
    const offset = (page - 1) * pageSize;
    return {
        list: all.slice(offset, offset + pageSize),
        total: all.length,
        page,
        pageSize,
        totalPages: Math.ceil(all.length / pageSize),
    };
}

export function selectDnsLines(dnsZone: DnsZoneItem): DnsLineItem[] {
    return dnsZone.runtime.lines.map((line) => ({
        line_code: line.code,
        line_name: line.display_name || line.name || `未知线路（${line.code}）`,
        line_display_name: line.display_name,
        status: line.status,
        last_synced_at: dnsZone.runtime.lines_synced_at,
    }));
}
