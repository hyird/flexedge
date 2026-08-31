import { Drawer } from 'antd';
import { createContext, useContext, type ComponentProps, type ReactNode } from 'react';
import { cn } from '@/lib/utils';
type SheetState = { open?: boolean; onOpenChange?: (open: boolean) => void };
type DismissEvent = { preventDefault(): void; readonly defaultPrevented: boolean };
type SheetContentProps = {
    side?: 'top' | 'right' | 'bottom' | 'left';
    className?: string;
    children?: ReactNode;
    showCloseButton?: boolean;
    onEscapeKeyDown?: (event: DismissEvent) => void;
    onInteractOutside?: (event: DismissEvent) => void;
    onPointerDownOutside?: (event: DismissEvent) => void;
};
const SheetContext = createContext<SheetState>({});
export function Sheet({ open, onOpenChange, children }: SheetState & { children?: ReactNode }) {
    return <SheetContext.Provider value={{ open, onOpenChange }}>{children}</SheetContext.Provider>;
}
export function SheetContent({
    side = 'right',
    className,
    children,
    showCloseButton = true,
    onEscapeKeyDown,
    onInteractOutside,
    onPointerDownOutside,
}: SheetContentProps) {
    const state = useContext(SheetContext);
    const placement = side;
    const horizontal = side === 'left' || side === 'right';
    return (
        <Drawer
            open={state.open}
            onClose={(event) => {
                if ('key' in event && event.key === 'Escape') onEscapeKeyDown?.(event);
                else {
                    onPointerDownOutside?.(event);
                    onInteractOutside?.(event);
                }
                if (!event.defaultPrevented) state.onOpenChange?.(false);
            }}
            placement={placement}
            closable={showCloseButton}
            width={horizontal ? 'min(860px, 92vw)' : undefined}
            height={!horizontal ? '80vh' : undefined}
            styles={{
                body: { padding: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' },
            }}
            className={className}
        >
            {children}
        </Drawer>
    );
}
export function SheetHeader({ className, ...props }: ComponentProps<'div'>) {
    return (
        <div
            className={cn('flex shrink-0 flex-col gap-1.5 border-b px-6 py-4', className)}
            {...props}
        />
    );
}
export function SheetFooter({ className, ...props }: ComponentProps<'div'>) {
    return (
        <div
            className={cn('mt-auto flex shrink-0 justify-end gap-2 border-t px-6 py-4', className)}
            {...props}
        />
    );
}
export function SheetTitle({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('text-base font-semibold', className)} {...props} />;
}
export function SheetDescription({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('text-sm text-gray-500', className)} {...props} />;
}
export function SheetTrigger({ children }: { children?: ReactNode; asChild?: boolean }) {
    return <>{children}</>;
}
export function SheetClose({ children }: { children?: ReactNode; asChild?: boolean }) {
    return <>{children}</>;
}
