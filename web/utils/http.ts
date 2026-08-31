/**
 * HTTP 请求基础配置
 */

import axios, {
    type AxiosError,
    type AxiosInstance,
    type AxiosRequestConfig,
    type AxiosResponse,
} from 'axios';
import { toast } from '@/components/ui/notification';
import {
    ApiError,
    type ApiResponseEnvelope,
    buildApiError,
    buildApiErrorFromResponse,
    decodeApiDataPayload,
    decodeApiOperationPayload,
} from './api.response';
import { AuthRefreshCoordinator } from './auth_refresh';

function normalizePath(path: string) {
    return path.startsWith('/') ? path : `/${path}`;
}

function getHashRoutePath(path: string) {
    const baseUrl = (import.meta.env.BASE_URL || '/').replace(/\/$/, '');
    return `${baseUrl || ''}/#${normalizePath(path)}`;
}

function redirectToLogin() {
    if (window.location.hash === '#/login') {
        return;
    }
    window.location.replace(getHashRoutePath('/login'));
}

export type RequestConfig = AxiosRequestConfig;

export interface RevisionedResourceRef {
    id: string;
    revision: number;
}

export interface RevisionedConfigSnapshot<TConfig> extends RevisionedResourceRef {
    config: TConfig;
}

export function withExpectedRevision(revision: number): RequestConfig {
    return { headers: { 'If-Match': `"${revision}"` } };
}

const transport = axios.create({
    baseURL: '/',
    timeout: 30000,
    withCredentials: true,
});

export interface HttpAuthSession {
    refresh: () => Promise<boolean>;
    clear: () => void;
}

let authSession: Readonly<HttpAuthSession> | undefined;

export function configureHttpAuth(session: HttpAuthSession) {
    if (authSession) throw new Error('HTTP auth session is already configured');
    authSession = Object.freeze({ ...session });
}

function buildApiErrorFromAxiosError(error: AxiosError<unknown>) {
    if (!error.response) {
        const isTimeout = error.code === 'ECONNABORTED';
        return buildApiError(isTimeout ? '请求超时' : '网络连接失败', {
            status: isTimeout ? 408 : 0,
            data: error,
            source: isTimeout ? 'timeout' : 'network',
        });
    }

    return buildApiErrorFromResponse(
        {
            data: error.response.data,
            status: error.response.status,
        },
        error.message || '请求失败'
    );
}

function notifyTransportIssue(error: AxiosError<unknown>) {
    if (error.code === 'ECONNABORTED') {
        toast.error('请求超时', {
            description: '网络连接较慢，请稍后重试',
        });
        return;
    }

    toast.error('网络连接失败', {
        description: '请检查网络连接是否正常',
    });
}

function createExpiredAuthError() {
    return buildApiError('登录状态已失效，请重新登录', {
        status: 401,
        source: 'auth-refresh',
    });
}

const refreshCoordinator = new AuthRefreshCoordinator();

function handleAuthExpired(error: ApiError): never {
    refreshCoordinator.fail(error);
    authSession?.clear();
    redirectToLogin();
    throw error;
}

transport.interceptors.response.use(
    (response) => response,
    async (error: AxiosError<unknown>): Promise<AxiosResponse> => {
        const originalRequest = error.config;
        const requestUrl = originalRequest?.url || '';
        const isSilent = originalRequest?._silent ?? false;
        const isAuthRequest = requestUrl.includes('/api/auth/');

        if (isSilent) {
            throw buildApiErrorFromAxiosError(error);
        }

        if (
            error.response?.status === 401 &&
            originalRequest &&
            !originalRequest._retry &&
            !isAuthRequest
        ) {
            originalRequest._retry = true;
            if (!refreshCoordinator.tryStart()) {
                await refreshCoordinator.wait();
                return transport.request(originalRequest);
            }

            let refreshed = false;
            try {
                refreshed = (await authSession?.refresh()) ?? false;
            } catch {
                return handleAuthExpired(createExpiredAuthError());
            }
            if (!refreshed) return handleAuthExpired(createExpiredAuthError());
            refreshCoordinator.succeed();
            return transport.request(originalRequest);
        }

        const apiError = buildApiErrorFromAxiosError(error);

        if (!error.response) {
            notifyTransportIssue(error);
            throw apiError;
        }

        if (error.response.status >= 500) {
            toast.error('请求失败', {
                description: apiError.message || '服务器遇到问题，请稍后重试',
            });
            throw buildApiError(apiError.message || '服务器错误', {
                code: apiError.code,
                status: apiError.status ?? error.response.status,
                data: apiError.data,
                source: 'server',
            });
        }

        toast.error(apiError.message || error.message || '请求失败');
        throw apiError;
    }
);

export interface ApiClient {
    get<T = unknown>(url: string, config?: RequestConfig): Promise<T>;
    getRaw<T>(url: string, config?: RequestConfig): Promise<T>;
    post<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
    postOperation(url: string, data?: unknown, config?: RequestConfig): Promise<void>;
    putOperation(url: string, data?: unknown, config?: RequestConfig): Promise<void>;
    deleteOperation(url: string, config?: RequestConfig): Promise<void>;
}

async function decodeDataResponse<T>(
    responsePromise: Promise<AxiosResponse<ApiResponseEnvelope<T>>>
) {
    const response = await responsePromise;
    try {
        return decodeApiDataPayload<T>(response.data, response.status);
    } catch (error) {
        if (error instanceof ApiError && !response.config._silent) toast.error(error.message);
        throw error;
    }
}

async function decodeOperationResponse(
    responsePromise: Promise<AxiosResponse<ApiResponseEnvelope<void>>>
) {
    const response = await responsePromise;
    try {
        return decodeApiOperationPayload(response.data, response.status);
    } catch (error) {
        if (error instanceof ApiError && !response.config._silent) toast.error(error.message);
        throw error;
    }
}

export function createApiClient(clientTransport: AxiosInstance): ApiClient {
    return {
        get: <T = unknown>(url: string, config?: RequestConfig) =>
            decodeDataResponse(clientTransport.get<ApiResponseEnvelope<T>>(url, config)),
        getRaw: async <T>(url: string, config?: RequestConfig) =>
            (await clientTransport.get<T>(url, config)).data,
        post: <T = unknown>(url: string, data?: unknown, config?: RequestConfig) =>
            decodeDataResponse(clientTransport.post<ApiResponseEnvelope<T>>(url, data, config)),
        postOperation: (url: string, data?: unknown, config?: RequestConfig) =>
            decodeOperationResponse(
                clientTransport.post<ApiResponseEnvelope<void>>(url, data, config)
            ),
        putOperation: (url: string, data?: unknown, config?: RequestConfig) =>
            decodeOperationResponse(
                clientTransport.put<ApiResponseEnvelope<void>>(url, data, config)
            ),
        deleteOperation: (url: string, config?: RequestConfig) =>
            decodeOperationResponse(clientTransport.delete<ApiResponseEnvelope<void>>(url, config)),
    };
}

const request = createApiClient(transport);

export default request;
