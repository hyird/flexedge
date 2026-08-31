import { describe, expect, test } from 'bun:test';
import { formatDateTime } from './date';

describe('formatDateTime', () => {
    test('formats PostgreSQL microsecond timestamps with hour-only offsets', () => {
        const value = formatDateTime('2026-08-24T13:39:06.572319+08');
        expect(value).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
    });

    test('keeps invalid values visible for diagnostics', () => {
        expect(formatDateTime('invalid timestamp')).toBe('invalid timestamp');
    });
});
