import { describe, expect, test } from 'bun:test';
import { mergeLogTail } from './log_tail';

function log(id) {
    return { id };
}

describe('log tail', () => {
    test('prepends updates, removes duplicates, and advances the cursor', () => {
        const merged = mergeLogTail(
            { cursor: '10:a', list: [log('a'), log('b')] },
            { cursor: '20:c', list: [log('c'), log('a')] },
            100
        );

        expect(merged.cursor).toBe('20:c');
        expect(merged.list.map((item) => item.id)).toEqual(['c', 'a', 'b']);
    });

    test('retains the cursor on an empty update and caps the selected window', () => {
        const current = {
            cursor: '10:a',
            list: Array.from({ length: 101 }, (_, index) => log(String(index))),
        };
        const merged = mergeLogTail(current, { list: [] }, 100);

        expect(merged.cursor).toBe('10:a');
        expect(merged.list).toHaveLength(100);
        expect(merged.list.at(-1)?.id).toBe('99');
    });
});
