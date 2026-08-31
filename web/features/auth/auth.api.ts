/**
 * 认证传输边界
 */

import request, { type RequestConfig } from '@/utils/http';
import type { AuthSessionResult, LoginRequest, UserInfo } from './auth.types';

/** API 端点 */
const ENDPOINTS = {
    LOGIN: '/api/auth/login',
    REFRESH: '/api/auth/refresh',
    ME: '/api/auth/me',
    LOGOUT: '/api/auth/logout',
} as const;

/** 登录 */
export function login(params: LoginRequest) {
    return request.post<AuthSessionResult>(ENDPOINTS.LOGIN, params);
}

/** 刷新服务端会话 */
export function refreshSession(config?: RequestConfig) {
    return request.post<AuthSessionResult>(ENDPOINTS.REFRESH, undefined, config);
}

/** 获取当前用户信息 */
export function fetchCurrentUser(config?: RequestConfig) {
    return request.get<UserInfo>(ENDPOINTS.ME, config);
}

/** 登出 */
export function logout(config?: RequestConfig) {
    return request.postOperation(ENDPOINTS.LOGOUT, undefined, config);
}
