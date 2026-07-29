/*
 * Timer and scheduler validation for the native Emu68 target.
 */

#include "boot.h"

#include <devices/timer.h>
#include <exec/errors.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

extern volatile ULONG emu68_vtimer_ticks;

static volatile ULONG worker_counter_a;
static volatile ULONG worker_counter_b;

static void scheduler_worker_a(void)
{
    for (;;)
        worker_counter_a++;
}

static void scheduler_worker_b(void)
{
    for (;;)
        worker_counter_b++;
}

static struct timerequest *open_timer_request(struct MsgPort *port)
{
    struct timerequest *request;

    request = (struct timerequest *)
        CreateIORequest(port, sizeof(struct timerequest));
    if (!request)
        return NULL;

    if (OpenDevice(TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)request, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)request);
        return NULL;
    }

    return request;
}

static void close_timer_request(struct timerequest *request)
{
    if (request)
    {
        CloseDevice((struct IORequest *)request);
        DeleteIORequest((struct IORequest *)request);
    }
}

static ULONG timer_device_probe(void)
{
    struct MsgPort *port;
    struct timerequest *request;
    struct timeval before;
    ULONG result = 0;

    port = CreateMsgPort();
    if (!port)
        return 0;

    request = open_timer_request(port);
    if (request)
    {
        request->tr_node.io_Command = TR_GETSYSTIME;
        DoIO((struct IORequest *)request);
        if (request->tr_time.tv_secs != 0 ||
            request->tr_time.tv_micro != 0)
        {
            LONG elapsed_secs;
            LONG elapsed_micro;

            before = request->tr_time;
            result |= EMU68_BOOT_TIMER_TICKING;
            emu68_boot_context.flags |= EMU68_BOOT_TIMER_TICKING;
            emu68_set_stage(EMU68_STAGE_TIMER_DEVICE);
            emu68_console_puts(
                "[AROS/Emu68] timer.device clock is advancing\n");

            request->tr_node.io_Command = TR_ADDREQUEST;
            request->tr_time.tv_secs = 0;
            request->tr_time.tv_micro = 40000;
            if (DoIO((struct IORequest *)request) == 0)
            {
                result |= EMU68_BOOT_TIMER_WAKEUP;
                request->tr_node.io_Command = TR_GETSYSTIME;
                DoIO((struct IORequest *)request);
                elapsed_secs = (LONG)request->tr_time.tv_secs -
                               (LONG)before.tv_secs;
                elapsed_micro = (LONG)request->tr_time.tv_micro -
                                (LONG)before.tv_micro;
                if (elapsed_micro < 0)
                {
                    elapsed_secs--;
                    elapsed_micro += 1000000;
                }
                if (elapsed_secs > 0 ||
                    (elapsed_secs == 0 && elapsed_micro >= 40000))
                    result |= EMU68_BOOT_TIMER_COHERENT;
            }
        }
        close_timer_request(request);
    }
    DeleteMsgPort(port);
    return result;
}

static ULONG timer_device_concurrency_probe(void)
{
    struct MsgPort *port;
    struct timerequest *short_request = NULL;
    struct timerequest *long_request = NULL;
    ULONG before_a;
    ULONG before_b;
    ULONG result = 0;

    port = CreateMsgPort();
    if (!port)
        return 0;

    short_request = open_timer_request(port);
    long_request = open_timer_request(port);
    if (!short_request || !long_request)
        goto out;

    before_a = worker_counter_a;
    before_b = worker_counter_b;
    short_request->tr_node.io_Command = TR_ADDREQUEST;
    short_request->tr_time.tv_secs = 0;
    short_request->tr_time.tv_micro = 40000;
    long_request->tr_node.io_Command = TR_ADDREQUEST;
    long_request->tr_time.tv_secs = 0;
    long_request->tr_time.tv_micro = 80000;

    SendIO((struct IORequest *)long_request);
    SendIO((struct IORequest *)short_request);
    if (WaitIO((struct IORequest *)short_request) == 0 &&
        CheckIO((struct IORequest *)long_request) == NULL)
        result |= EMU68_BOOT_TIMER_CONCURRENT;
    if (WaitIO((struct IORequest *)long_request) != 0)
        goto out;
    if (worker_counter_a != before_a && worker_counter_b != before_b)
        result |= EMU68_BOOT_TIMER_PREEMPT;

    long_request->tr_node.io_Command = TR_ADDREQUEST;
    long_request->tr_time.tv_secs = 5;
    long_request->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)long_request);
    if (AbortIO((struct IORequest *)long_request) == 0 &&
        WaitIO((struct IORequest *)long_request) == IOERR_ABORTED)
        result |= EMU68_BOOT_TIMER_ABORT;

out:
    close_timer_request(long_request);
    close_timer_request(short_request);
    DeleteMsgPort(port);
    return result;
}

static BOOL timer_device_soak_probe(void)
{
    struct MsgPort *port;
    struct timerequest *request;
    ULONG second;
    BOOL passed = TRUE;

    port = CreateMsgPort();
    if (!port)
        return FALSE;
    request = open_timer_request(port);
    if (!request)
    {
        DeleteMsgPort(port);
        return FALSE;
    }

    emu68_set_stage(EMU68_STAGE_TIMER_SOAK);
    emu68_console_puts("[AROS/Emu68] starting two-minute timer soak\n");
    for (second = 1; second <= 120; second++)
    {
        ULONG before_a = worker_counter_a;
        ULONG before_b = worker_counter_b;

        request->tr_node.io_Command = TR_ADDREQUEST;
        request->tr_time.tv_secs = 1;
        request->tr_time.tv_micro = 0;
        if (DoIO((struct IORequest *)request) != 0)
        {
            emu68_console_puts(
                "[AROS/Emu68] timer soak: timer request failed\n");
            passed = FALSE;
            break;
        }
        if (worker_counter_a == before_a)
        {
            emu68_console_puts(
                "[AROS/Emu68] timer soak: worker A was starved\n");
            passed = FALSE;
            break;
        }
        if (worker_counter_b == before_b)
        {
            emu68_console_puts(
                "[AROS/Emu68] timer soak: worker B was starved\n");
            passed = FALSE;
            break;
        }

        emu68_boot_context.timer_soak_seconds = second;
        if (second == 60)
        {
            emu68_set_stage(EMU68_STAGE_TIMER_MINUTE1);
            emu68_console_puts("[AROS/Emu68] timer soak: one minute\n");
        }
        else if (second == 120)
        {
            emu68_set_stage(EMU68_STAGE_TIMER_MINUTE2);
            emu68_console_puts("[AROS/Emu68] timer soak: two minutes\n");
        }
    }

    close_timer_request(request);
    DeleteMsgPort(port);
    return passed && emu68_boot_context.timer_soak_seconds == 120;
}

static void scheduler_probe(void)
{
    ULONG timer_result;
    ULONG concurrency_result;

    emu68_boot_context.flags |= EMU68_BOOT_TASK_RUNNING;
    emu68_set_stage(EMU68_STAGE_TASK_RUNNING);
    emu68_console_puts("[AROS/Emu68] scheduled task is running\n");
    while (emu68_vtimer_ticks < 3)
        ;

    emu68_set_stage(EMU68_STAGE_TIMER_RUNNING);
    timer_result = timer_device_probe();
    if (timer_result & EMU68_BOOT_TIMER_WAKEUP)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_WAKEUP;
        emu68_set_stage(EMU68_STAGE_TIMER_WAKEUP);
        emu68_console_puts("[AROS/Emu68] timer.device woke the task\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] timer.device request failed\n");

    if (timer_result & EMU68_BOOT_TIMER_COHERENT)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_COHERENT;
        emu68_set_stage(EMU68_STAGE_TIMER_COHERENT);
        emu68_console_puts(
            "[AROS/Emu68] timer.device elapsed time is coherent\n");
    }
    else
        emu68_console_puts(
            "[AROS/Emu68] timer.device elapsed time is invalid\n");

    emu68_console_puts("[AROS/Emu68] virtual timer interrupts are running\n");
    concurrency_result = timer_device_concurrency_probe();
    if (concurrency_result & EMU68_BOOT_TIMER_CONCURRENT)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_CONCURRENT;
        emu68_set_stage(EMU68_STAGE_TIMER_CONCURRENT);
        emu68_console_puts(
            "[AROS/Emu68] simultaneous timer requests completed in order\n");
    }
    else
        emu68_console_puts(
            "[AROS/Emu68] simultaneous timer request test failed\n");

    if (concurrency_result & EMU68_BOOT_TIMER_ABORT)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_ABORT;
        emu68_set_stage(EMU68_STAGE_TIMER_ABORT);
        emu68_console_puts("[AROS/Emu68] AbortIO cancelled timer request\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] AbortIO timer test failed\n");

    if (concurrency_result & EMU68_BOOT_TIMER_PREEMPT)
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_PREEMPT;

    if (timer_device_soak_probe())
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_SOAK;
        emu68_set_stage(EMU68_STAGE_TIMER_COMPLETE);
        emu68_console_puts(
            "[AROS/Emu68] timer/scheduler soak completed successfully\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] timer/scheduler soak failed\n");

    for (;;)
        ;
}

int emu68_scheduler_selftest_start(void)
{
    BOOL result = TRUE;

    Forbid();
    if (!NewCreateTask(TASKTAG_NAME, "Emu68 timer worker A",
                       TASKTAG_PRI, 10,
                       TASKTAG_PC, scheduler_worker_a,
                       TAG_DONE) ||
        !NewCreateTask(TASKTAG_NAME, "Emu68 timer worker B",
                       TASKTAG_PRI, 10,
                       TASKTAG_PC, scheduler_worker_b,
                       TAG_DONE))
        result = FALSE;

    if (!NewCreateTask(TASKTAG_NAME, "Emu68 scheduler probe",
                       TASKTAG_PRI, 125,
                       TASKTAG_PC, scheduler_probe,
                       TAG_DONE))
        result = FALSE;
    Permit();

    return result;
}
