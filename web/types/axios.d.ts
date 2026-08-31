import 'axios';

declare module 'axios' {
    interface AxiosRequestConfig {
        _retry?: boolean;
        _silent?: boolean;
    }
}
