import ReactDOM from 'react-dom/client';
import { HashRouter } from 'react-router-dom';
import { StyleProvider } from '@ant-design/cssinjs';
import { App, ConfigProvider } from 'antd';
import zhCN from 'antd/locale/zh_CN';
import { ErrorBoundary } from './components/ErrorBoundary';
import { NotificationBridge } from './components/ui/notification';
import { APP_NAME } from './config/app';
import { httpAuthSession } from './features/auth/auth.session';
import { TanstackQuery } from './providers/TanstackQuery';
import { AppRoutes } from './routes';
import { configureHttpAuth } from './utils/http';
import './styles/index.css';

configureHttpAuth(httpAuthSession);

const rootElement = document.getElementById('root');
if (!rootElement) throw new Error('Root element #root not found in DOM');

document.title = APP_NAME;

ReactDOM.createRoot(rootElement).render(
    <StyleProvider layer>
        <ConfigProvider
            locale={zhCN}
            theme={{
                cssVar: { prefix: 'ant', key: 'flexedge' },
                token: {
                    colorPrimary: '#1677ff',
                    borderRadius: 6,
                    fontFamily:
                        'Inter Variable, -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif',
                },
            }}
        >
            <App>
                <ErrorBoundary>
                    <TanstackQuery>
                        <HashRouter>
                            <AppRoutes />
                        </HashRouter>
                    </TanstackQuery>
                    <NotificationBridge />
                </ErrorBoundary>
            </App>
        </ConfigProvider>
    </StyleProvider>
);
