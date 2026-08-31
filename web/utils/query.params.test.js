import { describe, expect, test } from 'bun:test';
import { appendQueryParams } from './query.params';

describe('appendQueryParams', () => {
    test('owns camelCase to snake_case conversion and URL encoding', () => {
        expect(
            appendQueryParams('/api/resources', {
                pageSize: 20,
                ownerOf: '*.example.com',
                keyword: 'edge & origin',
            })
        ).toBe('/api/resources?page_size=20&owner_of=*.example.com&keyword=edge+%26+origin');
    });

    test('omits absent values without dropping false or zero', () => {
        expect(
            appendQueryParams('/api/resources', {
                empty: '',
                missing: undefined,
                nullable: null,
                available: false,
                page: 0,
            })
        ).toBe('/api/resources?available=false&page=0');
    });

    test('appends to an existing query string with one separator', () => {
        expect(appendQueryParams('/api/resources?status=enabled', { page: 2 })).toBe(
            '/api/resources?status=enabled&page=2'
        );
    });
});
