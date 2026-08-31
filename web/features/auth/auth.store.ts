import { create } from 'zustand';
import type { UserInfo } from './auth.types';

interface AuthState {
    user: UserInfo | null;
    initialized: boolean;
}

interface AuthActions {
    clearAuth: () => void;
    setUser: (user: UserInfo) => void;
}

export type AuthStore = AuthState & AuthActions;

export const useAuthStore = create<AuthStore>((set) => ({
    user: null,
    initialized: false,

    clearAuth: () => {
        set({ user: null, initialized: true });
    },

    setUser: (user) => {
        set({ user, initialized: true });
    },
}));
