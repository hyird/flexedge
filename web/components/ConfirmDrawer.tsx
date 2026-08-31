import { Check, LoaderCircle, Trash2, TriangleAlert, X } from 'lucide-react';
import { type ReactNode, useEffect, useState } from 'react';
import {
    AlertDialog,
    AlertDialogContent,
    AlertDialogDescription,
    AlertDialogFooter,
    AlertDialogHeader,
    AlertDialogMedia,
    AlertDialogTitle,
} from '@/components/ui/alert_dialog';
import { Button } from '@/components/ui/button';

export interface ConfirmDrawerAction {
    title: string;
    content?: ReactNode;
    confirmText?: string;
    danger?: boolean;
    onConfirm: () => void | Promise<void>;
}

interface ConfirmDrawerProps {
    action?: ConfirmDrawerAction;
    onClose: () => void;
}

export default function ConfirmDrawer({ action, onClose }: ConfirmDrawerProps) {
    const [confirming, setConfirming] = useState(false);

    useEffect(() => {
        if (!action) setConfirming(false);
    }, [action]);

    const confirm = async () => {
        if (!action || confirming) return;
        setConfirming(true);
        try {
            await action.onConfirm();
            onClose();
        } catch {
            setConfirming(false);
            return;
        }
        setConfirming(false);
    };

    return (
        <AlertDialog
            open={Boolean(action)}
            onOpenChange={(open) => {
                if (!open && !confirming) onClose();
            }}
        >
            <AlertDialogContent
                onEscapeKeyDown={(event) => {
                    if (confirming) event.preventDefault();
                }}
            >
                <AlertDialogHeader>
                    <AlertDialogMedia
                        className={
                            action?.danger ? 'bg-destructive/10 text-destructive' : undefined
                        }
                    >
                        {action?.danger ? <TriangleAlert /> : <Check />}
                    </AlertDialogMedia>
                    <AlertDialogTitle>{action?.title ?? '确认操作'}</AlertDialogTitle>
                    <AlertDialogDescription asChild>
                        <div>{action?.content ?? '确认执行此操作？'}</div>
                    </AlertDialogDescription>
                </AlertDialogHeader>
                <AlertDialogFooter>
                    <Button variant="outline" disabled={confirming} onClick={onClose}>
                        <X />
                        取消
                    </Button>
                    <Button
                        variant={action?.danger ? 'destructive' : 'default'}
                        disabled={confirming}
                        onClick={confirm}
                    >
                        {confirming ? (
                            <LoaderCircle className="animate-spin" />
                        ) : action?.danger ? (
                            <Trash2 />
                        ) : (
                            <Check />
                        )}
                        {action?.confirmText ?? '确认'}
                    </Button>
                </AlertDialogFooter>
            </AlertDialogContent>
        </AlertDialog>
    );
}
