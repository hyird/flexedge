import { Modal } from 'antd';
import { createContext, useContext, type ComponentProps, type ReactNode } from 'react';
import { cn } from '@/lib/utils';
type State = { open?: boolean; onOpenChange?: (open: boolean) => void };
const Context = createContext<State>({});
export function AlertDialog({ open, onOpenChange, children }: State & { children?: ReactNode }) {
    return <Context.Provider value={{ open, onOpenChange }}>{children}</Context.Provider>;
}
export function AlertDialogContent({
    children,
    className,
}: {
    children?: ReactNode;
    className?: string;
    onEscapeKeyDown?: (event: KeyboardEvent) => void;
}) {
    const state = useContext(Context);
    return (
        <Modal
            open={state.open}
            onCancel={() => state.onOpenChange?.(false)}
            footer={null}
            width={440}
            className={className}
        >
            {children}
        </Modal>
    );
}
export function AlertDialogHeader({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('text-center', className)} {...props} />;
}
export function AlertDialogFooter({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('mt-6 flex justify-end gap-2', className)} {...props} />;
}
export function AlertDialogTitle({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('mt-3 text-lg font-semibold', className)} {...props} />;
}
export function AlertDialogDescription({
    className,
    ...props
}: ComponentProps<'div'> & { asChild?: boolean }) {
    return <div className={cn('mt-2 text-sm text-gray-500', className)} {...props} />;
}
export function AlertDialogMedia({ className, ...props }: ComponentProps<'div'>) {
    return (
        <div
            className={cn(
                'mx-auto flex size-12 items-center justify-center rounded-full bg-blue-50 text-blue-600',
                className
            )}
            {...props}
        />
    );
}
export function AlertDialogTrigger({ children }: { children?: ReactNode }) {
    return <>{children}</>;
}
export function AlertDialogAction({ children }: { children?: ReactNode }) {
    return <>{children}</>;
}
export function AlertDialogCancel({ children }: { children?: ReactNode }) {
    return <>{children}</>;
}
export function AlertDialogOverlay() {
    return null;
}
export function AlertDialogPortal({ children }: { children?: ReactNode }) {
    return <>{children}</>;
}
