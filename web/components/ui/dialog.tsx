import { Modal } from 'antd';
import { createContext, useContext, type ComponentProps, type ReactNode } from 'react';
import { cn } from '@/lib/utils';
type DialogState = { open?: boolean; onOpenChange?: (open: boolean) => void };
type DismissEvent = { preventDefault(): void; readonly defaultPrevented: boolean };
const DialogContext = createContext<DialogState>({});
export function Dialog({ open, onOpenChange, children }: DialogState & { children?: ReactNode }) {
    return (
        <DialogContext.Provider value={{ open, onOpenChange }}>{children}</DialogContext.Provider>
    );
}
export function DialogContent({
    className,
    children,
    showCloseButton = true,
    onEscapeKeyDown,
    onInteractOutside,
    onPointerDownOutside,
}: {
    className?: string;
    children?: ReactNode;
    showCloseButton?: boolean;
    onEscapeKeyDown?: (event: DismissEvent) => void;
    onInteractOutside?: (event: DismissEvent) => void;
    onPointerDownOutside?: (event: DismissEvent) => void;
}) {
    const state = useContext(DialogContext);
    return (
        <Modal
            open={state.open}
            onCancel={(event) => {
                if ('key' in event && event.key === 'Escape') onEscapeKeyDown?.(event);
                else {
                    onPointerDownOutside?.(event);
                    onInteractOutside?.(event);
                }
                if (!event.defaultPrevented) state.onOpenChange?.(false);
            }}
            footer={null}
            closable={showCloseButton}
            width={760}
            className={className}
            styles={{ body: { maxHeight: '80vh', overflow: 'auto' } }}
        >
            {children}
        </Modal>
    );
}
export function DialogHeader({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('mb-5 flex flex-col gap-1', className)} {...props} />;
}
export function DialogFooter({
    className,
    children,
    ...props
}: ComponentProps<'div'> & { showCloseButton?: boolean }) {
    return (
        <div className={cn('mt-6 flex justify-end gap-2', className)} {...props}>
            {children}
        </div>
    );
}
export function DialogTitle({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('text-base font-semibold', className)} {...props} />;
}
export function DialogDescription({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('text-sm text-gray-500', className)} {...props} />;
}
export function DialogTrigger({ children }: { children?: ReactNode; asChild?: boolean }) {
    return <>{children}</>;
}
export function DialogClose({ children }: { children?: ReactNode; asChild?: boolean }) {
    return <>{children}</>;
}
export function DialogPortal({ children }: { children?: ReactNode }) {
    return <>{children}</>;
}
export function DialogOverlay() {
    return null;
}
