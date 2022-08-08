/*
 * WAYLANDDRV initialization code
 *
 * Copyright 2020 Alexandre Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"
#include "wine/server.h"

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

/* We use use pointer to call NtWaitForMultipleObjects to make it go through
 * syscall dispatcher. We need that because win32u bypasses syscall thunks and
 * if we called NtWaitForMultipleObjects directly, it wouldn't be able to handle
 * user APCs. This will be removed as soon as we may use syscall interface
 * for NtUserMsgWaitForMultipleObjectsEx. */
NTSTATUS (WINAPI *pNtWaitForMultipleObjects)(ULONG, const HANDLE *, BOOLEAN,
                                             BOOLEAN, const LARGE_INTEGER*);

static NTSTATUS CDECL waylanddrv_unix_call(enum waylanddrv_unix_func func, void *params);

static void set_queue_fd(struct wayland *wayland)
{
    HANDLE handle;
    int wfd;
    int ret;

    wfd = wayland->event_notification_pipe[0];

    if (wine_server_fd_to_handle(wfd, GENERIC_READ | SYNCHRONIZE, 0, &handle))
    {
        ERR("Can't allocate handle for wayland fd\n");
        NtTerminateProcess(0, 1);
    }

    SERVER_START_REQ(set_queue_fd)
    {
        req->handle = wine_server_obj_handle(handle);
        ret = wine_server_call(req);
    }
    SERVER_END_REQ;

    if (ret)
    {
        ERR("Can't store handle for wayland fd %x\n", ret);
        NtTerminateProcess(0, 1);
    }

    NtClose(handle);
}

/***********************************************************************
 *           Initialize per thread data
 */
struct wayland_thread_data *wayland_init_thread_data(void)
{
    struct wayland_thread_data *data = wayland_thread_data();

    if (data) return data;

    if (!(data = calloc(1, sizeof(*data))))
    {
        ERR("could not create data\n");
        NtTerminateProcess(0, 1);
    }

    if (!wayland_init(&data->wayland))
    {
        ERR_(winediag)("waylanddrv: Can't open wayland display. Please ensure "
                       "that your wayland server is running and that "
                       "$WAYLAND_DISPLAY is set correctly.\n");
        NtTerminateProcess(0, 1);
    }

    set_queue_fd(&data->wayland);
    NtUserGetThreadInfo()->driver_data = data;

    /* Create the clipboard window outside of thread init. We delay window
     * creation since the thread init function may be invoked from within the
     * context of a user32 function which holds the internal Wine user32 lock.
     * In such a case creating the clipboard window would cause an internal
     * user32 lock error. */
    NtUserPostThreadMessage(data->wayland.thread_id,
                            WM_WAYLAND_CLIPBOARD_WINDOW_CREATE, 0, 0);

    return data;
}

/***********************************************************************
 *           ThreadDetach (WAYLAND.@)
 */
static void WAYLAND_ThreadDetach(void)
{
    struct wayland_thread_data *data = wayland_thread_data();

    if (data)
    {
        wayland_deinit(&data->wayland);
        free(data);
        /* clear data in case we get re-entered from user32 before the thread is truly dead */
        NtUserGetThreadInfo()->driver_data = NULL;
    }
}

const struct user_driver_funcs waylanddrv_funcs =
{
    .dc_funcs.pCreateDC = WAYLAND_CreateDC,
    .dc_funcs.pCreateCompatibleDC = WAYLAND_CreateCompatibleDC,
    .dc_funcs.pDeleteDC = WAYLAND_DeleteDC,
    .dc_funcs.pPutImage = WAYLAND_PutImage,
    .dc_funcs.priority = GDI_PRIORITY_GRAPHICS_DRV,

    .pChangeDisplaySettingsEx = WAYLAND_ChangeDisplaySettingsEx,
    .pClipCursor = WAYLAND_ClipCursor,
    .pCreateWindow = WAYLAND_CreateWindow,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pEnumDisplaySettingsEx = WAYLAND_EnumDisplaySettingsEx,
    .pGetKeyNameText = WAYLAND_GetKeyNameText,
    .pMapVirtualKeyEx = WAYLAND_MapVirtualKeyEx,
    .pMsgWaitForMultipleObjectsEx = WAYLAND_MsgWaitForMultipleObjectsEx,
    .pSetCursor = WAYLAND_SetCursor,
    .pSetLayeredWindowAttributes = WAYLAND_SetLayeredWindowAttributes,
    .pSetWindowRgn = WAYLAND_SetWindowRgn,
    .pSetWindowStyle = WAYLAND_SetWindowStyle,
    .pSetWindowText = WAYLAND_SetWindowText,
    .pShowWindow = WAYLAND_ShowWindow,
    .pSysCommand = WAYLAND_SysCommand,
    .pToUnicodeEx = WAYLAND_ToUnicodeEx,
    .pUpdateLayeredWindow = WAYLAND_UpdateLayeredWindow,
    .pVkKeyScanEx = WAYLAND_VkKeyScanEx,
    .pThreadDetach = WAYLAND_ThreadDetach,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
    .pwine_get_vulkan_driver = WAYLAND_wine_get_vulkan_driver,
    .pwine_get_wgl_driver = WAYLAND_wine_get_wgl_driver,
};

static NTSTATUS waylanddrv_unix_init(void *arg)
{
    struct waylanddrv_unix_init_params *params = arg;

    pNtWaitForMultipleObjects = params->pNtWaitForMultipleObjects;

    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    wayland_read_options_from_registry();

    wayland_data_device_init_formats();

    if (!wayland_init_set_cursor()) return STATUS_UNSUCCESSFUL;

    if (!wayland_process_init()) return STATUS_UNSUCCESSFUL;

    params->unix_call = waylanddrv_unix_call;
    params->option_show_systray = option_show_systray;

    return 0;
}

static NTSTATUS waylanddrv_unix_read_events(void *arg)
{
    while (wayland_read_events_and_dispatch_process()) continue;
    /* This function only returns on a fatal error, e.g., if our connection
     * to the Wayland server is lost. */
    return STATUS_UNSUCCESSFUL;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_clipboard_message,
    waylanddrv_unix_data_offer_accept_format,
    waylanddrv_unix_data_offer_enum_formats,
    waylanddrv_unix_data_offer_import_format,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == waylanddrv_unix_func_count);

/* FIXME: Use __wine_unix_call instead */
static NTSTATUS CDECL waylanddrv_unix_call(enum waylanddrv_unix_func func, void *params)
{
    return __wine_unix_call_funcs[func](params);
}

/***********************************************************************
 *           waylanddrv_client_call
 */
NTSTATUS waylanddrv_client_call(enum waylanddrv_client_func func, const void *params,
                                ULONG size)
{
    void *ret_ptr;
    ULONG ret_len;
    return KeUserModeCallback(func, params, size, &ret_ptr, &ret_len);
}
