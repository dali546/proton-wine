/*
 * Wayland driver DLL definitions
 *
 * Copyright 2022 Alexandros Frantzis for Collabora Ltd
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

#ifndef __WINE_WAYLANDDRV_DLL_H
#define __WINE_WAYLANDDRV_DLL_H

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"

#include "unixlib.h"

/* FIXME: Use __wine_unix_call instead */
extern NTSTATUS (CDECL *waylanddrv_unix_call)(enum waylanddrv_unix_func func, void *params) DECLSPEC_HIDDEN;
#define WAYLANDDRV_UNIX_CALL(func, params) waylanddrv_unix_call(waylanddrv_unix_func_ ## func, params)
extern BOOL option_show_systray DECLSPEC_HIDDEN;

NTSTATUS WINAPI waylanddrv_client_create_clipboard_window(void *arg, ULONG size);
NTSTATUS WINAPI waylanddrv_client_dnd(void *params, ULONG size);

#endif /* __WINE_WAYLANDDRV_DLL_H */
