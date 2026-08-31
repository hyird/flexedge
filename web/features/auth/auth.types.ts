/**
 * 认证会话类型定义
 */

// ============ 登录相关 ============

export interface LoginRequest {
    username: string;
    password: string;
}

export interface AuthSessionResult {
    user: UserInfo;
}

// ============ 用户信息 ============

export interface UserInfo {
    id: string;
    username: string;
    nickname?: string;
    status: 'enabled' | 'disabled';
}
