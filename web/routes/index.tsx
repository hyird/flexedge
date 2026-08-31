import { lazy, Suspense } from 'react';
import { Navigate, Outlet, Route, Routes, useLocation } from 'react-router-dom';
import { LoadingScreen } from '@/components/ui/spinner';
import { useAuthStore } from '@/features/auth/auth.store';
import { useInitAuth } from '@/hooks/useInitAuth';

const LoginPage = lazy(() => import('@/pages/auth'));
const OverviewPage = lazy(() => import('@/pages/overview'));
const DnsProviderPage = lazy(() => import('@/pages/provider/dns_provider_page'));
const DnsZonePage = lazy(() => import('@/pages/dns_zone'));
const CertificatePage = lazy(() => import('@/pages/certificate'));
const ClusterPage = lazy(() => import('@/pages/cluster'));
const WebsitePage = lazy(() => import('@/pages/website'));
const AdminLayout = lazy(() => import('@/layouts/AdminLayout'));

function AuthGuard() {
    const user = useAuthStore((state) => state.user);
    const initialized = useAuthStore((state) => state.initialized);
    const location = useLocation();
    if (!initialized) return <LoadingScreen />;
    if (!user) return <Navigate to="/login" state={{ from: location }} replace />;
    return <Outlet />;
}

export function AppRoutes() {
    useInitAuth();
    return (
        <Suspense fallback={<LoadingScreen />}>
            <Routes>
                <Route path="/login" element={<LoginPage />} />
                <Route element={<AuthGuard />}>
                    <Route element={<AdminLayout />}>
                        <Route index element={<Navigate to="/overview" replace />} />
                        <Route path="overview" element={<OverviewPage />} />
                        <Route path="dns-providers" element={<DnsProviderPage />} />
                        <Route path="dns-zones" element={<DnsZonePage />} />
                        <Route path="certificates" element={<CertificatePage />} />
                        <Route path="clusters" element={<ClusterPage />} />
                        <Route path="websites" element={<WebsitePage />} />
                        <Route path="nodes" element={<Navigate to="/clusters" replace />} />
                    </Route>
                </Route>
                <Route path="*" element={<Navigate to="/" replace />} />
            </Routes>
        </Suspense>
    );
}
