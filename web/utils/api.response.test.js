import { describe, expect, test } from 'bun:test';
import { ApiError, decodeApiDataPayload, decodeApiOperationPayload } from './api.response';

describe('API response decoding', () => {
    test('unwraps a successful API envelope', () => {
        expect(
            decodeApiDataPayload({ code: 0, message: 'ok', data: { id: 'resource-1' } }, 200)
        ).toEqual({ id: 'resource-1' });
    });

    test('accepts a successful operation response without data', () => {
        expect(decodeApiOperationPayload({ code: 0, message: 'saved' }, 200)).toBeUndefined();
    });

    test('rejects non-envelope payloads at the API boundary', () => {
        const payload = new Blob(['certificate']);
        expect(() => decodeApiDataPayload(payload, 200)).toThrow('服务器响应格式不正确');
    });

    test('requires data only for data responses', () => {
        expect(() => decodeApiDataPayload({ code: 0, message: 'ok' }, 200)).toThrow(
            '服务器响应缺少 data'
        );
        expect(() =>
            decodeApiOperationPayload({ code: 0, message: 'saved', data: null }, 200)
        ).toThrow('命令响应不应包含 data');
    });

    test('turns a failed API envelope into the shared error type', () => {
        const payload = { code: 16801, message: 'revision conflict', data: { current: 4 } };

        try {
            decodeApiDataPayload(payload, 409);
            throw new Error('expected decodeApiDataPayload to throw');
        } catch (error) {
            expect(error).toBeInstanceOf(ApiError);
            expect(error).toMatchObject({
                message: 'revision conflict',
                code: 16801,
                status: 409,
                source: 'response',
                data: payload,
            });
        }
    });
});
