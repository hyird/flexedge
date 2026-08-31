import { describe, expect, test } from 'bun:test';
import axios from 'axios';
import { ApiError } from './api.response';
import { createApiClient } from './http';

function createTransport(data, status = 200) {
    return axios.create({
        adapter: async (config) => ({
            data,
            status,
            statusText: 'OK',
            headers: {},
            config,
        }),
    });
}

describe('createApiClient', () => {
    test('unwraps business data without changing the Axios transport contract', async () => {
        const client = createApiClient(
            createTransport({ code: 0, message: 'ok', data: { id: 'resource-1' } })
        );
        await expect(client.get('/resource-1')).resolves.toEqual({ id: 'resource-1' });
    });

    test('returns raw download payloads only through the explicit raw boundary', async () => {
        const payload = new Blob(['certificate']);
        const client = createApiClient(createTransport(payload));
        await expect(client.getRaw('/certificate.crt')).resolves.toBe(payload);
        await expect(client.get('/certificate.crt')).rejects.toMatchObject({
            name: ApiError.name,
            message: '服务器响应格式不正确',
            status: 200,
        });
    });

    test('rejects a failed business envelope with ApiError', async () => {
        const client = createApiClient(
            createTransport({ code: 16801, message: 'revision conflict' }, 409)
        );
        await expect(client.putOperation('/resource-1')).rejects.toMatchObject({
            name: ApiError.name,
            code: 16801,
            status: 409,
        });
    });

    test('keeps data and operation response contracts distinct', async () => {
        await expect(
            createApiClient(createTransport({ code: 0, message: 'ok' })).get('/resource-1')
        ).rejects.toMatchObject({ message: '服务器响应缺少 data' });
        await expect(
            createApiClient(
                createTransport({ code: 0, message: 'saved', data: { id: 'resource-1' } })
            ).postOperation('/resource-1')
        ).rejects.toMatchObject({ message: '命令响应不应包含 data' });
    });
});
