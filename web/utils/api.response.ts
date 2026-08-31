export type ApiErrorSource = 'response' | 'network' | 'timeout' | 'server' | 'auth-refresh';

export interface ApiResponseEnvelope<T = unknown> {
    code: number;
    message: string;
    data?: T;
}

export interface ApiErrorOptions {
    code?: number;
    status?: number;
    data?: unknown;
    source?: ApiErrorSource;
}

export class ApiError extends Error {
    code?: number;
    status?: number;
    data?: unknown;
    source: ApiErrorSource;

    constructor(message: string, options: ApiErrorOptions = {}) {
        super(message);
        this.name = 'ApiError';
        this.code = options.code;
        this.status = options.status;
        this.data = options.data;
        this.source = options.source ?? 'response';

        Object.setPrototypeOf(this, ApiError.prototype);
    }
}

export function isApiResponseEnvelope(value: unknown): value is ApiResponseEnvelope {
    if (typeof value !== 'object' || value === null) return false;
    return (
        'code' in value &&
        typeof value.code === 'number' &&
        'message' in value &&
        typeof value.message === 'string'
    );
}

export function getApiResponseMessage(value: unknown, fallback = '请求失败'): string {
    if (!isApiResponseEnvelope(value)) return fallback;

    const message = value.message.trim();
    return message || fallback;
}

export function getApiResponseCode(value: unknown): number | undefined {
    return isApiResponseEnvelope(value) ? value.code : undefined;
}

export function buildApiError(
    message: string,
    options: {
        code?: number;
        status?: number;
        data?: unknown;
        source?: ApiError['source'];
    } = {}
) {
    return new ApiError(message, options);
}

export function buildApiErrorFromResponse(
    response: { data: unknown; status: number },
    fallbackMessage = '请求失败'
) {
    const payload = response.data;
    const message = getApiResponseMessage(payload, fallbackMessage);
    const code = getApiResponseCode(payload);

    return buildApiError(message, {
        code,
        status: response.status,
        data: payload,
        source: 'response',
    });
}

function requireApiEnvelope(payload: unknown, status: number): ApiResponseEnvelope {
    if (!isApiResponseEnvelope(payload))
        throw buildApiError('服务器响应格式不正确', {
            status,
            data: payload,
            source: 'response',
        });
    if (payload.code !== 0) throw buildApiErrorFromResponse({ data: payload, status });
    return payload;
}

export function decodeApiDataPayload<T>(payload: unknown, status: number): T {
    const envelope = requireApiEnvelope(payload, status);
    if (!Object.hasOwn(envelope, 'data'))
        throw buildApiError('服务器响应缺少 data', {
            status,
            data: payload,
            source: 'response',
        });
    return envelope.data as T;
}

export function decodeApiOperationPayload(payload: unknown, status: number): void {
    const envelope = requireApiEnvelope(payload, status);
    if (Object.hasOwn(envelope, 'data'))
        throw buildApiError('命令响应不应包含 data', {
            status,
            data: payload,
            source: 'response',
        });
}
