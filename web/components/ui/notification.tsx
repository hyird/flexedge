import { App } from 'antd';

type ToastOptions = { description?: string };
type ToastKind = 'success' | 'error' | 'info' | 'warning';
let messageApi: ReturnType<typeof App.useApp>['message'] | undefined;
let notificationApi: ReturnType<typeof App.useApp>['notification'] | undefined;

function show(type: ToastKind, content: string, options?: ToastOptions) {
    if (options?.description) {
        notificationApi?.[type]({ message: content, description: options.description });
        return;
    }
    messageApi?.[type](content);
}

export const toast = {
    success: (content: string, options?: ToastOptions) => show('success', content, options),
    error: (content: string, options?: ToastOptions) => show('error', content, options),
    info: (content: string, options?: ToastOptions) => show('info', content, options),
    warning: (content: string, options?: ToastOptions) => show('warning', content, options),
};

export function NotificationBridge() {
    const app = App.useApp();
    messageApi = app.message;
    notificationApi = app.notification;
    return null;
}
