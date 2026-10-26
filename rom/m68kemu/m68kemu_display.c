/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    m68kemu.library — native window presenter for Rigel frames.

    Opens a window on the default public screen at the first completed
    frame, copies every frame into it, paces the program to the chipset's
    frame rate, feeds the window's keyboard and mouse to the chipset, and
    stops the program on the close gadget or Ctrl-C.
*/

#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>

#include "m68kemu_intern.h"

#define RESYNC_FRAMES       5       /* drop pacing debt beyond this */

/* Rigel frames are host ULONG 0x00RRGGBB (Rigel ISSUE-0010) */
#if AROS_BIG_ENDIAN
#define FRAME_RECTFMT       RECTFMT_ARGB32
#else
#define FRAME_RECTFMT       RECTFMT_BGRA32
#endif

struct M68KEmuDisplay
{
    struct Window      *win;
    BOOL                win_failed;
    ULONG               width;      /* frame size the window was sized for */
    ULONG               height;
    BOOL                left;       /* mouse buttons as last sent */
    BOOL                right;
    BOOL                have_mouse; /* mouse_x/y hold a position */
    WORD                mouse_x;
    WORD                mouse_y;
    struct MsgPort     *timer_port;
    struct timerequest *timer_req;
    BOOL                timer_open;
    BOOL                timer_tried;
    struct timeval      next;
    BOOL                have_next;
};

static void tv_add_us(struct timeval *tv, ULONG us)
{
    tv->tv_micro += us;
    tv->tv_secs  += tv->tv_micro / 1000000;
    tv->tv_micro %= 1000000;
}

/* a - b in microseconds, saturated; negative when b is later */
static LONG tv_diff_us(const struct timeval *a, const struct timeval *b)
{
    LONG secs = (LONG)a->tv_secs - (LONG)b->tv_secs;

    if (secs > 2000)
        return 0x7fffffff;
    if (secs < -2000)
        return -0x7fffffff;
    return secs * 1000000 + ((LONG)a->tv_micro - (LONG)b->tv_micro);
}

/*
 * Hold the program to one frame per frame_us of wall-clock time. Deadlines
 * are absolute, so a late frame is made up by the next ones.
 */
static void pace(struct M68KEmuDisplay *d, ULONG frame_us)
{
    struct timeval now;
    LONG ahead;

    if (!d->timer_open || !frame_us)
        return;

    d->timer_req->tr_node.io_Command = TR_GETSYSTIME;
    DoIO((struct IORequest *)d->timer_req);
    now = d->timer_req->tr_time;

    if (!d->have_next)
    {
        d->next = now;
        d->have_next = TRUE;
    }

    ahead = tv_diff_us(&d->next, &now);
    if (ahead > 0)
    {
        d->timer_req->tr_node.io_Command = TR_ADDREQUEST;
        d->timer_req->tr_time.tv_secs  = (ULONG)ahead / 1000000;
        d->timer_req->tr_time.tv_micro = (ULONG)ahead % 1000000;
        DoIO((struct IORequest *)d->timer_req);
    }
    else if (-ahead > (LONG)(RESYNC_FRAMES * frame_us))
    {
        /* Too far behind to catch up: start counting from now */
        d->next = now;
    }
    tv_add_us(&d->next, frame_us);
}

/* Feed window input to the chipset; FALSE when the user asked to stop */
static BOOL handle_input(struct M68KEmuContext *ctx, struct M68KEmuDisplay *d)
{
    struct IntuiMessage *msg;
    BOOL keep = TRUE;
    BOOL left = d->left, right = d->right;

    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        keep = FALSE;

    if (!d->win)
        return keep;
    while ((msg = (struct IntuiMessage *)GetMsg(d->win->UserPort)))
    {
        switch (msg->Class)
        {
        case IDCMP_CLOSEWINDOW:
            keep = FALSE;
            break;
        case IDCMP_RAWKEY:
            /* Amiga raw keycodes; the chipset sees no auto-repeat */
            if (!(msg->Qualifier & IEQUALIFIER_REPEAT))
                M68KEmu_RigelInputKey(ctx, (UBYTE)msg->Code);
            break;
        case IDCMP_MOUSEBUTTONS:
            if (msg->Code == SELECTDOWN)    left = TRUE;
            else if (msg->Code == SELECTUP) left = FALSE;
            else if (msg->Code == MENUDOWN) right = TRUE;
            else if (msg->Code == MENUUP)   right = FALSE;
            break;
        case IDCMP_MOUSEMOVE:
            if (d->have_mouse)
                M68KEmu_RigelInputMouse(ctx, msg->MouseX - d->mouse_x,
                                        msg->MouseY - d->mouse_y);
            d->mouse_x = msg->MouseX;
            d->mouse_y = msg->MouseY;
            d->have_mouse = TRUE;
            break;
        }
        ReplyMsg((struct Message *)msg);
    }

    if (left != d->left || right != d->right)
    {
        M68KEmu_RigelInputButtons(ctx, left, right);
        d->left = left;
        d->right = right;
    }
    return keep;
}

/* Task-bound resources are created by the task that runs the program */
static void open_timer(struct M68KEmuDisplay *d)
{
    d->timer_tried = TRUE;
    d->timer_port = CreateMsgPort();
    if (d->timer_port)
        d->timer_req = (struct timerequest *)CreateIORequest(
            d->timer_port, sizeof(struct timerequest));
    if (d->timer_req &&
        OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)d->timer_req, 0) == 0)
        d->timer_open = TRUE;
    else
        bug("[m68kemu] display: no timer.device, frames are not paced\n");
}

static BOOL display_present(struct M68KEmuContext *ctx,
                            const struct M68KEmuFrame *frame)
{
    struct M68KEmuDisplay *d = (struct M68KEmuDisplay *)ctx->presenter.data;

    if (!d->timer_tried)
        open_timer(d);

    if (!d->win && !d->win_failed)
    {
        d->win = OpenWindowTags(NULL,
            WA_Title,       (IPTR)"m68kemu: Rigel display",
            WA_InnerWidth,  frame->width,
            WA_InnerHeight, frame->height,
            WA_IDCMP,       IDCMP_CLOSEWINDOW | IDCMP_RAWKEY |
                            IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,
            WA_ReportMouse, TRUE,
            WA_CloseGadget, TRUE,
            WA_DragBar,     TRUE,
            WA_DepthGadget, TRUE,
            WA_Activate,    TRUE,
            WA_RMBTrap,     TRUE,
            TAG_DONE);
        if (!d->win)
        {
            /* Keep running headless rather than failing the program */
            bug("[m68kemu] display: cannot open a %lux%lu window\n",
                (unsigned long)frame->width, (unsigned long)frame->height);
            d->win_failed = TRUE;
        }
    }

    else if (d->win && (frame->width != d->width || frame->height != d->height))
    {
        /* The frame size follows the program's display window, which is
           usually set up after the first frame */
        ChangeWindowBox(d->win, d->win->LeftEdge, d->win->TopEdge,
            frame->width + d->win->BorderLeft + d->win->BorderRight,
            frame->height + d->win->BorderTop + d->win->BorderBottom);
    }
    d->width  = frame->width;
    d->height = frame->height;

    if (d->win)
    {
        /* Resizing is asynchronous: draw what fits until it is done */
        ULONG w = frame->width, h = frame->height;
        ULONG max_w = d->win->Width - d->win->BorderLeft - d->win->BorderRight;
        ULONG max_h = d->win->Height - d->win->BorderTop - d->win->BorderBottom;

        if (w > max_w) w = max_w;
        if (h > max_h) h = max_h;
        WritePixelArray((APTR)frame->pixels, 0, 0, frame->pitch, d->win->RPort,
                        d->win->BorderLeft, d->win->BorderTop, w, h,
                        FRAME_RECTFMT);
    }

    if (!handle_input(ctx, d))
        return FALSE;
    pace(d, frame->frame_us);
    return TRUE;
}

static void display_destroy(struct M68KEmuContext *ctx)
{
    struct M68KEmuDisplay *d = (struct M68KEmuDisplay *)ctx->presenter.data;

    if (!d)
        return;
    if (d->win)
    {
        struct IntuiMessage *msg;
        while ((msg = (struct IntuiMessage *)GetMsg(d->win->UserPort)))
            ReplyMsg((struct Message *)msg);
        CloseWindow(d->win);
    }
    if (d->timer_open)
        CloseDevice((struct IORequest *)d->timer_req);
    if (d->timer_req)
        DeleteIORequest((struct IORequest *)d->timer_req);
    if (d->timer_port)
        DeleteMsgPort(d->timer_port);
    FreeVec(d);

    ctx->presenter.present = NULL;
    ctx->presenter.destroy = NULL;
    ctx->presenter.data    = NULL;
}

/*
 * Attach the window presenter. The window and timer are opened at the
 * first frame by the task running the program (possibly a child process),
 * and released when M68KEmu_Execute() returns, in that same task; a
 * program that never completes a frame opens neither.
 */
BOOL M68KEmu_DisplayAttach(struct M68KEmuContext *ctx)
{
    struct M68KEmuDisplay *d;

    d = AllocVec(sizeof(*d), MEMF_CLEAR);
    if (!d)
        return FALSE;

    ctx->presenter.data    = d;
    ctx->presenter.present = display_present;
    ctx->presenter.destroy = display_destroy;
    return TRUE;
}
