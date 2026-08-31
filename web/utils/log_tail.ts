import { decodeApiDataPayload } from './api.response';

export type LogTailLimit = 100 | 1000 | 10000;

export interface LogTail<T extends { id: string }> {
    list: T[];
    cursor?: string;
}

export const LOG_TAIL_LIMIT_OPTIONS: Array<{ label: string; value: LogTailLimit }> = [
    { label: '100', value: 100 },
    { label: '1000', value: 1000 },
    { label: '10000', value: 10000 },
];

export function mergeLogTail<T extends { id: string }>(
    current: LogTail<T> | undefined,
    update: LogTail<T>,
    limit: LogTailLimit
): LogTail<T> {
    const incomingIds = new Set(update.list.map((item) => item.id));
    return {
        cursor: update.cursor ?? current?.cursor,
        list: [
            ...update.list,
            ...(current?.list.filter((item) => !incomingIds.has(item.id)) ?? []),
        ].slice(0, limit),
    };
}

export function decodeLogTailEvent<T extends { id: string }>(
    event: MessageEvent<string>
): LogTail<T> | undefined {
    try {
        return decodeApiDataPayload<LogTail<T>>(JSON.parse(event.data), 200);
    } catch {
        return undefined;
    }
}
