import type { SyncStatus } from './dns_zone.types';

export const dnsZoneSyncStatusMeta: Record<
    SyncStatus,
    { tone: 'info' | 'success' | 'warning' | 'destructive'; label: string }
> = {
    pending: { tone: 'info', label: '等待同步' },
    synced: { tone: 'success', label: '已同步' },
    conflict: { tone: 'warning', label: '等待处理冲突' },
    failed: { tone: 'destructive', label: '同步失败' },
};
