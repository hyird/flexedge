import { RefreshCw, RotateCcw, TriangleAlert } from 'lucide-react';
import { Component, type ErrorInfo, type ReactNode } from 'react';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardFooter, CardHeader, CardTitle } from '@/components/ui/card';
import { getAppTitle } from '@/config/app';

interface ErrorBoundaryProps {
    children: ReactNode;
}

interface ErrorBoundaryState {
    hasError: boolean;
    error: Error | null;
}

export class ErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
    state: ErrorBoundaryState = { hasError: false, error: null };

    static getDerivedStateFromError(error: Error): ErrorBoundaryState {
        return { hasError: true, error };
    }

    componentDidCatch(error: Error, info: ErrorInfo) {
        console.error('[ErrorBoundary]', error, info.componentStack);
    }

    handleReset = () => {
        this.setState({ hasError: false, error: null });
    };

    handleReload = () => {
        window.location.reload();
    };

    render() {
        if (this.state.hasError) {
            return (
                <div className="flex min-h-dvh items-center justify-center bg-muted/30 p-6">
                    <Card className="w-full max-w-lg" role="alert">
                        <CardHeader className="items-center text-center">
                            <span className="mb-2 flex size-12 items-center justify-center rounded-full bg-destructive/10 text-destructive">
                                <TriangleAlert className="size-6" />
                            </span>
                            <CardTitle className="text-xl">{getAppTitle('页面出现异常')}</CardTitle>
                        </CardHeader>
                        <CardContent>
                            <p className="break-words text-center text-sm text-muted-foreground">
                                {this.state.error?.message || '未知错误'}
                            </p>
                        </CardContent>
                        <CardFooter className="justify-center gap-2">
                            <Button onClick={this.handleReset}>
                                <RotateCcw />
                                重试
                            </Button>
                            <Button variant="outline" onClick={this.handleReload}>
                                <RefreshCw />
                                刷新页面
                            </Button>
                        </CardFooter>
                    </Card>
                </div>
            );
        }

        return this.props.children;
    }
}
