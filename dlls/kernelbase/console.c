/*
 * Win32 console functions
 *
 * Copyright 1995 Martin von Loewis and Cameron Heide
 * Copyright 1997 Karl Garrison
 * Copyright 1998 John Richardson
 * Copyright 1998 Marcus Meissner
 * Copyright 2001,2002,2004,2005,2010 Eric Pouech
 * Copyright 2001 Alexandre Julliard
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define NONAMELESSUNION
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"
#include "wincon.h"
#include "wine/condrv.h"
#include "wine/server.h"
#include "wine/exception.h"
#include "wine/debug.h"
#include "kernelbase.h"

WINE_DEFAULT_DEBUG_CHANNEL(console);


static CRITICAL_SECTION console_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &console_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": console_section") }
};
static CRITICAL_SECTION console_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static HANDLE console_connection;
static HANDLE console_wait_event;
static unsigned int console_flags;

#define CONSOLE_INPUT_HANDLE    0x01
#define CONSOLE_OUTPUT_HANDLE   0x02
#define CONSOLE_ERROR_HANDLE    0x04

static WCHAR input_exe[MAX_PATH + 1];

struct ctrl_handler
{
    PHANDLER_ROUTINE     func;
    struct ctrl_handler *next;
};

static BOOL WINAPI default_ctrl_handler( DWORD type )
{
    FIXME( "Terminating process %x on event %x\n", GetCurrentProcessId(), type );
    RtlExitUserProcess( 0 );
    return TRUE;
}

static struct ctrl_handler default_handler = { default_ctrl_handler, NULL };
static struct ctrl_handler *ctrl_handlers = &default_handler;

static BOOL console_ioctl( HANDLE handle, DWORD code, void *in_buff, DWORD in_count,
                           void *out_buff, DWORD out_count, DWORD *read )
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtDeviceIoControlFile( handle, NULL, NULL, NULL, &io, code, in_buff, in_count,
                                    out_buff, out_count );
    switch( status )
    {
    case STATUS_SUCCESS:
        if (read) *read = io.Information;
        return TRUE;
    case STATUS_INVALID_PARAMETER:
        break;
    default:
        status = STATUS_INVALID_HANDLE;
        break;
    }
    if (read) *read = 0;
    return set_ntstatus( status );
}

/* map input records to ASCII */
static void input_records_WtoA( INPUT_RECORD *buffer, int count )
{
    UINT cp = GetConsoleCP();
    int i;
    char ch;

    for (i = 0; i < count; i++)
    {
        if (buffer[i].EventType != KEY_EVENT) continue;
        WideCharToMultiByte( cp, 0, &buffer[i].Event.KeyEvent.uChar.UnicodeChar, 1, &ch, 1, NULL, NULL );
        buffer[i].Event.KeyEvent.uChar.AsciiChar = ch;
    }
}

/* map input records to Unicode */
static void input_records_AtoW( INPUT_RECORD *buffer, int count )
{
    UINT cp = GetConsoleCP();
    int i;
    WCHAR ch;

    for (i = 0; i < count; i++)
    {
        if (buffer[i].EventType != KEY_EVENT) continue;
        MultiByteToWideChar( cp, 0, &buffer[i].Event.KeyEvent.uChar.AsciiChar, 1, &ch, 1 );
        buffer[i].Event.KeyEvent.uChar.UnicodeChar = ch;
    }
}

/* map char infos to ASCII */
static void char_info_WtoA( UINT cp, CHAR_INFO *buffer, int count )
{
    char ch;

    while (count-- > 0)
    {
        WideCharToMultiByte( cp, 0, &buffer->Char.UnicodeChar, 1, &ch, 1, NULL, NULL );
        buffer->Char.AsciiChar = ch;
        buffer++;
    }
}

/* map char infos to Unicode */
static void char_info_AtoW( CHAR_INFO *buffer, int count )
{
    UINT cp = GetConsoleOutputCP();
    WCHAR ch;

    while (count-- > 0)
    {
        MultiByteToWideChar( cp, 0, &buffer->Char.AsciiChar, 1, &ch, 1 );
        buffer->Char.UnicodeChar = ch;
        buffer++;
    }
}

/* helper function for GetLargestConsoleWindowSize */
static COORD get_largest_console_window_size( HANDLE handle )
{
    struct condrv_output_info info;
    COORD c = { 0, 0 };

    if (!console_ioctl( handle, IOCTL_CONDRV_GET_OUTPUT_INFO, NULL, 0, &info, sizeof(info), NULL ))
        return c;

    c.X = info.max_width;
    c.Y = info.max_height;
    TRACE( "(%p), returning %dx%d\n", handle, c.X, c.Y );
    return c;
}

static HANDLE create_console_server( void )
{
    OBJECT_ATTRIBUTES attr = {sizeof(attr)};
    UNICODE_STRING string;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;
    NTSTATUS status;

    RtlInitUnicodeString( &string, L"\\Device\\ConDrv\\Server" );
    attr.ObjectName = &string;
    attr.Attributes = OBJ_INHERIT;
    status = NtCreateFile( &handle, FILE_WRITE_PROPERTIES | FILE_READ_PROPERTIES | SYNCHRONIZE,
                           &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
    return set_ntstatus( status ) ? handle : NULL;
}

static HANDLE create_console_reference( HANDLE root )
{
    OBJECT_ATTRIBUTES attr = {sizeof(attr)};
    UNICODE_STRING string;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;
    NTSTATUS status;

    RtlInitUnicodeString( &string, L"Reference" );
    attr.RootDirectory = root;
    attr.ObjectName = &string;
    status = NtCreateFile( &handle, FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_PROPERTIES |
                           FILE_READ_PROPERTIES | SYNCHRONIZE, &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                           0, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
    return set_ntstatus( status ) ? handle : NULL;
}

static BOOL create_console_connection( HANDLE root )
{
    OBJECT_ATTRIBUTES attr = {sizeof(attr)};
    UNICODE_STRING string;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    RtlInitUnicodeString( &string, root ? L"Connection" : L"\\Device\\ConDrv\\Connection" );
    attr.RootDirectory = root;
    attr.ObjectName = &string;
    status = NtCreateFile( &console_connection, FILE_WRITE_PROPERTIES | FILE_READ_PROPERTIES | SYNCHRONIZE, &attr,
                           &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0 );
    return set_ntstatus( status );
}

static BOOL init_console_std_handles( BOOL override_all )
{
    HANDLE std_out = NULL, std_err = NULL, handle;
    OBJECT_ATTRIBUTES attr = {sizeof(attr)};
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING name;
    NTSTATUS status;

    attr.ObjectName = &name;
    attr.Attributes = OBJ_INHERIT;

    if (override_all || !GetStdHandle( STD_INPUT_HANDLE ))
    {
        /* FIXME: Use unbound console handle */
        RtlInitUnicodeString( &name, L"\\Device\\ConDrv\\CurrentIn" );
        status = NtCreateFile( &handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE | FILE_READ_ATTRIBUTES |
                               FILE_WRITE_ATTRIBUTES, &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
                               FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
        if (!set_ntstatus( status )) return FALSE;
        console_flags |= CONSOLE_INPUT_HANDLE;
        SetStdHandle( STD_INPUT_HANDLE, console_handle_map( handle ));
    }

    if (!override_all)
    {
        std_out = GetStdHandle( STD_OUTPUT_HANDLE );
        std_err = GetStdHandle( STD_ERROR_HANDLE );
        if (std_out && std_err) return TRUE;
    }

    /* FIXME: Use unbound console handle */
    RtlInitUnicodeString( &name, L"\\Device\\ConDrv\\CurrentOut" );
    status = NtCreateFile( &handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE | FILE_READ_ATTRIBUTES |
                           FILE_WRITE_ATTRIBUTES, &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
    if (!set_ntstatus( status )) return FALSE;
    if (!std_out)
    {
        console_flags |= CONSOLE_OUTPUT_HANDLE;
        SetStdHandle( STD_OUTPUT_HANDLE, console_handle_map( handle ));
    }

    if (!std_err)
    {
        if (!std_out && !DuplicateHandle( GetCurrentProcess(), handle, GetCurrentProcess(),
                                          &handle, 0, TRUE, DUPLICATE_SAME_ACCESS ))
            return FALSE;
        console_flags |= CONSOLE_ERROR_HANDLE;
        SetStdHandle( STD_ERROR_HANDLE, console_handle_map( handle ));
    }

    return TRUE;
}

/******************************************************************
 *	AttachConsole   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH AttachConsole( DWORD pid )
{
    BOOL ret;

    TRACE( "(%x)\n", pid );

    RtlEnterCriticalSection( &console_section );

    if (RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle)
    {
        RtlLeaveCriticalSection( &console_section );
        WARN( "console already attached\n" );
        SetLastError( ERROR_ACCESS_DENIED );
        return FALSE;
    }

    ret = create_console_connection( NULL ) &&
        console_ioctl( console_connection, IOCTL_CONDRV_BIND_PID, &pid, sizeof(pid), NULL, 0, NULL );
    if (ret)
    {
        RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle = create_console_reference( console_connection );
        if (RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle)
        {
            STARTUPINFOW si;
            GetStartupInfoW( &si );
            init_console_std_handles( !(si.dwFlags & STARTF_USESTDHANDLES) );
        }
        else ret = FALSE;
    }

    if (!ret) FreeConsole();
    RtlLeaveCriticalSection( &console_section );
    return ret;
}


/******************************************************************
 *      AllocConsole   (kernelbase.@)
 */
BOOL WINAPI AllocConsole(void)
{
    SECURITY_ATTRIBUTES inheritable_attr = { sizeof(inheritable_attr), NULL, TRUE };
    STARTUPINFOW app_si, console_si;
    WCHAR buffer[1024], cmd[256];
    PROCESS_INFORMATION pi;
    HANDLE event, console;
    BOOL ret;

    TRACE("()\n");

    RtlEnterCriticalSection( &console_section );

    if (RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle)
    {
        /* we already have a console opened on this process, don't create a new one */
        RtlLeaveCriticalSection( &console_section );
        SetLastError( ERROR_ACCESS_DENIED );
        return FALSE;
    }

    GetStartupInfoW(&app_si);

    memset(&console_si, 0, sizeof(console_si));
    console_si.cb = sizeof(console_si);
    /* setup a view arguments for wineconsole (it'll use them as default values)  */
    if (app_si.dwFlags & STARTF_USECOUNTCHARS)
    {
        console_si.dwFlags |= STARTF_USECOUNTCHARS;
        console_si.dwXCountChars = app_si.dwXCountChars;
        console_si.dwYCountChars = app_si.dwYCountChars;
    }
    if (app_si.dwFlags & STARTF_USEFILLATTRIBUTE)
    {
        console_si.dwFlags |= STARTF_USEFILLATTRIBUTE;
        console_si.dwFillAttribute = app_si.dwFillAttribute;
    }
    if (app_si.dwFlags & STARTF_USESHOWWINDOW)
    {
        console_si.dwFlags |= STARTF_USESHOWWINDOW;
        console_si.wShowWindow = app_si.wShowWindow;
    }
    if (app_si.lpTitle)
        console_si.lpTitle = app_si.lpTitle;
    else if (GetModuleFileNameW(0, buffer, ARRAY_SIZE(buffer)))
    {
        buffer[ARRAY_SIZE(buffer) - 1] = 0;
        console_si.lpTitle = buffer;
    }

    if (!(event = CreateEventW( &inheritable_attr, TRUE, FALSE, NULL ))) goto error;

    swprintf( cmd, ARRAY_SIZE(cmd),  L"wineconsole --use-event=%ld", (DWORD_PTR)event );
    if ((ret = CreateProcessW( NULL, cmd, NULL, NULL, TRUE, DETACHED_PROCESS, NULL, NULL, &console_si, &pi )))
    {
        HANDLE wait_handles[2] = { event, pi.hProcess };
        ret = WaitForMultipleObjects( ARRAY_SIZE(wait_handles), wait_handles, FALSE, INFINITE ) == WAIT_OBJECT_0;
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
    }
    CloseHandle( event );
    if (!ret || !init_console_std_handles( !(app_si.dwFlags & STARTF_USESTDHANDLES) )) goto error;
    console = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, 0, NULL, OPEN_EXISTING, 0, 0 );
    if (console == INVALID_HANDLE_VALUE) goto error;
    RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle = console;

    TRACE( "Started wineconsole pid=%08x tid=%08x\n", pi.dwProcessId, pi.dwThreadId );

    RtlLeaveCriticalSection( &console_section );
    SetLastError( ERROR_SUCCESS );
    return TRUE;

error:
    ERR("Can't allocate console\n");
    FreeConsole();
    RtlLeaveCriticalSection( &console_section );
    return FALSE;
}


/******************************************************************************
 *	CreateConsoleScreenBuffer   (kernelbase.@)
 */
HANDLE WINAPI DECLSPEC_HOTPATCH CreateConsoleScreenBuffer( DWORD access, DWORD share,
                                                           SECURITY_ATTRIBUTES *sa, DWORD flags,
                                                           void *data )
{
    OBJECT_ATTRIBUTES attr = {sizeof(attr)};
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING name;
    HANDLE handle;
    NTSTATUS status;

    TRACE( "(%x,%x,%p,%x,%p)\n", access, share, sa, flags, data );

    if (flags != CONSOLE_TEXTMODE_BUFFER || data)
    {
	SetLastError( ERROR_INVALID_PARAMETER );
	return INVALID_HANDLE_VALUE;
    }

    RtlInitUnicodeString( &name, L"\\Device\\ConDrv\\ScreenBuffer" );
    attr.ObjectName = &name;
    attr.SecurityDescriptor = sa ? sa->lpSecurityDescriptor : NULL;
    if (sa && sa->bInheritHandle) attr.Attributes |= OBJ_INHERIT;
    status = NtCreateFile( &handle, access, &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE, NULL, 0 );
    return set_ntstatus( status ) ? handle : INVALID_HANDLE_VALUE;
}


/******************************************************************************
 *	CtrlRoutine   (kernelbase.@)
 */
DWORD WINAPI CtrlRoutine( void *arg )
{
    DWORD_PTR event = (DWORD_PTR)arg;
    struct ctrl_handler *handler;

    if (event == CTRL_C_EVENT)
    {
        BOOL caught_by_dbg = TRUE;
        /* First, try to pass the ctrl-C event to the debugger (if any)
         * If it continues, there's nothing more to do
         * Otherwise, we need to send the ctrl-C event to the handlers
         */
        __TRY
        {
            RaiseException( DBG_CONTROL_C, 0, 0, NULL );
        }
        __EXCEPT_ALL
        {
            caught_by_dbg = FALSE;
        }
        __ENDTRY
        if (caught_by_dbg) return 0;
    }

    if (NtCurrentTeb()->Peb->ProcessParameters->ConsoleFlags & 1) return 0;

    RtlEnterCriticalSection( &console_section );
    for (handler = ctrl_handlers; handler; handler = handler->next)
    {
        if (handler->func( event )) break;
    }
    RtlLeaveCriticalSection( &console_section );
    return 1;
}


static LONG WINAPI handle_ctrl_c( EXCEPTION_POINTERS *eptr )
{
    if (eptr->ExceptionRecord->ExceptionCode != CONTROL_C_EXIT) return EXCEPTION_CONTINUE_SEARCH;
    if (!RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle)  return EXCEPTION_CONTINUE_SEARCH;

    if (!(NtCurrentTeb()->Peb->ProcessParameters->ConsoleFlags & 1))
    {
        HANDLE thread = CreateThread( NULL, 0, CtrlRoutine, (void*)CTRL_C_EVENT, 0, NULL );
        if (thread) CloseHandle( thread );
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}


/******************************************************************************
 *	FillConsoleOutputAttribute   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FillConsoleOutputAttribute( HANDLE handle, WORD attr, DWORD length,
                                                           COORD coord, DWORD *written )
{
    struct condrv_fill_output_params params;

    TRACE( "(%p,%d,%d,(%dx%d),%p)\n", handle, attr, length, coord.X, coord.Y, written );

    if (!written)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    *written = 0;

    params.mode  = CHAR_INFO_MODE_ATTR;
    params.x     = coord.X;
    params.y     = coord.Y;
    params.count = length;
    params.wrap  = TRUE;
    params.ch    = 0;
    params.attr  = attr;
    return console_ioctl( handle, IOCTL_CONDRV_FILL_OUTPUT, &params, sizeof(params),
                          written, sizeof(*written), NULL );
}


/******************************************************************************
 *	FillConsoleOutputCharacterA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FillConsoleOutputCharacterA( HANDLE handle, CHAR ch, DWORD length,
                                                           COORD coord, DWORD *written )
{
    WCHAR wch;

    MultiByteToWideChar( GetConsoleOutputCP(), 0, &ch, 1, &wch, 1 );
    return FillConsoleOutputCharacterW( handle, wch, length, coord, written );
}


/******************************************************************************
 *	FillConsoleOutputCharacterW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FillConsoleOutputCharacterW( HANDLE handle, WCHAR ch, DWORD length,
                                                           COORD coord, DWORD *written )
{
    struct condrv_fill_output_params params;

    TRACE( "(%p,%s,%d,(%dx%d),%p)\n", handle, debugstr_wn(&ch, 1), length, coord.X, coord.Y, written );

    if (!written)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    *written = 0;

    params.mode  = CHAR_INFO_MODE_TEXT;
    params.x     = coord.X;
    params.y     = coord.Y;
    params.count = length;
    params.wrap  = TRUE;
    params.ch    = ch;
    params.attr  = 0;
    return console_ioctl( handle, IOCTL_CONDRV_FILL_OUTPUT, &params, sizeof(params),
                          written, sizeof(*written), NULL );
}

HANDLE get_console_wait_handle( HANDLE handle )
{
    HANDLE event = 0;

    SERVER_START_REQ( get_console_wait_event )
    {
        req->handle = wine_server_obj_handle( console_handle_map( handle ));
        if (!wine_server_call( req )) event = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    if (event)
    {
        if (InterlockedCompareExchangePointer( &console_wait_event, event, 0 )) NtClose( event );
        handle = console_wait_event;
    }
    return handle;
}


/***********************************************************************
 *	FreeConsole   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH FreeConsole(void)
{
    HANDLE event;
    BOOL ret;

    RtlEnterCriticalSection( &console_section );

    NtClose( console_connection );
    console_connection = NULL;

    NtClose( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle );
    RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle = NULL;

    if (console_flags & CONSOLE_INPUT_HANDLE)  NtClose( GetStdHandle( STD_INPUT_HANDLE ));
    if (console_flags & CONSOLE_OUTPUT_HANDLE) NtClose( GetStdHandle( STD_OUTPUT_HANDLE ));
    if (console_flags & CONSOLE_ERROR_HANDLE)  NtClose( GetStdHandle( STD_ERROR_HANDLE ));
    console_flags = 0;

    SERVER_START_REQ( free_console )
    {
        ret = !wine_server_call_err( req );
    }
    SERVER_END_REQ;
    if ((event = InterlockedExchangePointer( &console_wait_event, NULL ))) NtClose( event );

    RtlLeaveCriticalSection( &console_section );
    return ret;
}


/******************************************************************************
 *	GenerateConsoleCtrlEvent   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GenerateConsoleCtrlEvent( DWORD event, DWORD group )
{
    struct condrv_ctrl_event ctrl_event;

    TRACE( "(%d, %x)\n", event, group );

    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
    {
	ERR( "Invalid event %d for PGID %x\n", event, group );
	return FALSE;
    }

    ctrl_event.event = event;
    ctrl_event.group_id = group;
    return console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle,
                          IOCTL_CONDRV_CTRL_EVENT, &ctrl_event, sizeof(ctrl_event), NULL, 0, NULL );
}


/******************************************************************************
 *	GetConsoleCP   (kernelbase.@)
 */
UINT WINAPI DECLSPEC_HOTPATCH GetConsoleCP(void)
{
    struct condrv_input_info info;

    if (!console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle,
                         IOCTL_CONDRV_GET_INPUT_INFO, NULL, 0, &info, sizeof(info), NULL ))
        return 0;
    return info.input_cp ? info.input_cp : GetOEMCP();
}


/******************************************************************************
 *	GetConsoleCursorInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetConsoleCursorInfo( HANDLE handle, CONSOLE_CURSOR_INFO *info )
{
    struct condrv_output_info condrv_info;

    if (!console_ioctl( handle, IOCTL_CONDRV_GET_OUTPUT_INFO, NULL, 0, &condrv_info, sizeof(condrv_info), NULL ))
        return FALSE;

    if (!info)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    info->dwSize   = condrv_info.cursor_size;
    info->bVisible = condrv_info.cursor_visible;
    TRACE("(%p) returning (%d,%d)\n", handle, info->dwSize, info->bVisible);
    return TRUE;
}


/***********************************************************************
 *	GetConsoleInputExeNameA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetConsoleInputExeNameA( DWORD len, LPSTR buffer )
{
    RtlEnterCriticalSection( &console_section );
    if (WideCharToMultiByte( CP_ACP, 0, input_exe, -1, NULL, 0, NULL, NULL ) <= len)
        WideCharToMultiByte( CP_ACP, 0, input_exe, -1, buffer, len, NULL, NULL );
    else SetLastError(ERROR_BUFFER_OVERFLOW);
    RtlLeaveCriticalSection( &console_section );
    return TRUE;
}


/***********************************************************************
 *	GetConsoleInputExeNameW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetConsoleInputExeNameW( DWORD len, LPWSTR buffer )
{
    RtlEnterCriticalSection( &console_section );
    if (len > lstrlenW(input_exe)) lstrcpyW( buffer, input_exe );
    else SetLastError( ERROR_BUFFER_OVERFLOW );
    RtlLeaveCriticalSection( &console_section );
    return TRUE;
}


/***********************************************************************
 *	GetConsoleMode   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetConsoleMode( HANDLE handle, DWORD *mode )
{
    return console_ioctl( handle, IOCTL_CONDRV_GET_MODE, NULL, 0, mode, sizeof(*mode), NULL );
}


/***********************************************************************
 *	GetConsoleOutputCP   (kernelbase.@)
 */
UINT WINAPI DECLSPEC_HOTPATCH GetConsoleOutputCP(void)
{
    struct condrv_input_info info;

    if (!console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle,
                         IOCTL_CONDRV_GET_INPUT_INFO, NULL, 0, &info, sizeof(info), NULL ))
        return 0;
    return info.output_cp ? info.output_cp : GetOEMCP();
}


/***********************************************************************
 *	GetConsoleScreenBufferInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetConsoleScreenBufferInfo( HANDLE handle, CONSOLE_SCREEN_BUFFER_INFO *info )
{
    struct condrv_output_info condrv_info;

    if (!console_ioctl( handle , IOCTL_CONDRV_GET_OUTPUT_INFO, NULL, 0,
                        &condrv_info, sizeof(condrv_info), NULL ))
        return FALSE;

    info->dwSize.X              = condrv_info.width;
    info->dwSize.Y              = condrv_info.height;
    info->dwCursorPosition.X    = condrv_info.cursor_x;
    info->dwCursorPosition.Y    = condrv_info.cursor_y;
    info->wAttributes           = condrv_info.attr;
    info->srWindow.Left         = condrv_info.win_left;
    info->srWindow.Right        = condrv_info.win_right;
    info->srWindow.Top          = condrv_info.win_top;
    info->srWindow.Bottom       = condrv_info.win_bottom;
    info->dwMaximumWindowSize.X = min(condrv_info.width, condrv_info.max_width);
    info->dwMaximumWindowSize.Y = min(condrv_info.height, condrv_info.max_height);

    TRACE( "(%p,(%d,%d) (%d,%d) %d (%d,%d-%d,%d) (%d,%d)\n", handle,
           info->dwSize.X, info->dwSize.Y, info->dwCursorPosition.X, info->dwCursorPosition.Y,
           info->wAttributes, info->srWindow.Left, info->srWindow.Top, info->srWindow.Right,
           info->srWindow.Bottom, info->dwMaximumWindowSize.X, info->dwMaximumWindowSize.Y );
    return TRUE;
}


/***********************************************************************
 *	GetConsoleScreenBufferInfoEx   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetConsoleScreenBufferInfoEx( HANDLE handle,
                                                            CONSOLE_SCREEN_BUFFER_INFOEX *info )
{
    struct condrv_output_info condrv_info;

    if (info->cbSize != sizeof(CONSOLE_SCREEN_BUFFER_INFOEX))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    if (!console_ioctl( handle, IOCTL_CONDRV_GET_OUTPUT_INFO, NULL, 0, &condrv_info,
                        sizeof(condrv_info), NULL ))
        return FALSE;

    info->dwSize.X              = condrv_info.width;
    info->dwSize.Y              = condrv_info.height;
    info->dwCursorPosition.X    = condrv_info.cursor_x;
    info->dwCursorPosition.Y    = condrv_info.cursor_y;
    info->wAttributes           = condrv_info.attr;
    info->srWindow.Left         = condrv_info.win_left;
    info->srWindow.Top          = condrv_info.win_top;
    info->srWindow.Right        = condrv_info.win_right;
    info->srWindow.Bottom       = condrv_info.win_bottom;
    info->dwMaximumWindowSize.X = min( condrv_info.width, condrv_info.max_width );
    info->dwMaximumWindowSize.Y = min( condrv_info.height, condrv_info.max_height );
    info->wPopupAttributes      = condrv_info.popup_attr;
    info->bFullscreenSupported  = FALSE;
    memcpy( info->ColorTable, condrv_info.color_map, sizeof(info->ColorTable) );
    return TRUE;
}


/******************************************************************************
 *	GetConsoleTitleW   (kernelbase.@)
 */
DWORD WINAPI DECLSPEC_HOTPATCH GetConsoleTitleW( LPWSTR title, DWORD size )
{
    if (!size) return 0;

    if (!console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle, IOCTL_CONDRV_GET_TITLE,
                        NULL, 0, title, (size - 1) * sizeof(WCHAR), &size ))
        return 0;

    size /= sizeof(WCHAR);
    title[size] = 0;
    return size + 1;
}


/***********************************************************************
 *	GetLargestConsoleWindowSize   (kernelbase.@)
 */
#if defined(__i386__) && !defined(__MINGW32__) && !defined(_MSC_VER)
#undef GetLargestConsoleWindowSize
DWORD WINAPI DECLSPEC_HOTPATCH GetLargestConsoleWindowSize( HANDLE handle )
{
    union {
	COORD c;
	DWORD w;
    } x;
    x.c = get_largest_console_window_size( handle );
    return x.w;
}

#else

COORD WINAPI DECLSPEC_HOTPATCH GetLargestConsoleWindowSize( HANDLE handle )
{
    return get_largest_console_window_size( handle );
}

#endif /* !defined(__i386__) */


/***********************************************************************
 *	GetNumberOfConsoleInputEvents   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetNumberOfConsoleInputEvents( HANDLE handle, DWORD *count )
{
    struct condrv_input_info info;
    if (!console_ioctl( handle, IOCTL_CONDRV_GET_INPUT_INFO, NULL, 0, &info, sizeof(info), NULL ))
        return FALSE;
    *count = info.input_count;
    return TRUE;
}


/***********************************************************************
 *	PeekConsoleInputA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH PeekConsoleInputA( HANDLE handle, INPUT_RECORD *buffer,
                                                 DWORD length, DWORD *count )
{
    DWORD read;

    if (!PeekConsoleInputW( handle, buffer, length, &read )) return FALSE;
    input_records_WtoA( buffer, read );
    if (count) *count = read;
    return TRUE;
}


/***********************************************************************
 *	PeekConsoleInputW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH PeekConsoleInputW( HANDLE handle, INPUT_RECORD *buffer,
                                                 DWORD length, DWORD *count )
{
    DWORD read;
    if (!console_ioctl( handle, IOCTL_CONDRV_PEEK, NULL, 0, buffer, length * sizeof(*buffer), &read ))
        return FALSE;
    if (count) *count = read / sizeof(*buffer);
    return TRUE;
}


/******************************************************************************
 *	ReadConsoleOutputAttribute   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ReadConsoleOutputAttribute( HANDLE handle, WORD *attr, DWORD length,
                                                          COORD coord, DWORD *count )
{
    struct condrv_output_params params;
    BOOL ret;

    TRACE( "(%p,%p,%d,%dx%d,%p)\n", handle, attr, length, coord.X, coord.Y, count );

    if (!count)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    params.mode  = CHAR_INFO_MODE_ATTR;
    params.x     = coord.X;
    params.y     = coord.Y;
    params.width = 0;
    ret = console_ioctl( handle, IOCTL_CONDRV_READ_OUTPUT, &params, sizeof(params),
                         attr, length * sizeof(*attr), count );
    *count /= sizeof(*attr);
    return ret;
}


/******************************************************************************
 *	ReadConsoleOutputCharacterA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ReadConsoleOutputCharacterA( HANDLE handle, LPSTR buffer, DWORD length,
                                                           COORD coord, DWORD *count )
{
    DWORD read;
    BOOL ret;
    LPWSTR wptr;

    if (!count)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    *count = 0;
    if (!(wptr = HeapAlloc( GetProcessHeap(), 0, length * sizeof(WCHAR) )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    if ((ret = ReadConsoleOutputCharacterW( handle, wptr, length, coord, &read )))
    {
        read = WideCharToMultiByte( GetConsoleOutputCP(), 0, wptr, read, buffer, length, NULL, NULL);
        *count = read;
    }
    HeapFree( GetProcessHeap(), 0, wptr );
    return ret;
}


/******************************************************************************
 *	ReadConsoleOutputCharacterW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ReadConsoleOutputCharacterW( HANDLE handle, LPWSTR buffer, DWORD length,
                                                           COORD coord, DWORD *count )
{
    struct condrv_output_params params;
    BOOL ret;

    TRACE( "(%p,%p,%d,%dx%d,%p)\n", handle, buffer, length, coord.X, coord.Y, count );

    if (!count)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    params.mode  = CHAR_INFO_MODE_TEXT;
    params.x     = coord.X;
    params.y     = coord.Y;
    params.width = 0;
    ret = console_ioctl( handle, IOCTL_CONDRV_READ_OUTPUT, &params, sizeof(params), buffer,
                         length * sizeof(*buffer), count );
    *count /= sizeof(*buffer);
    return ret;
}


/******************************************************************************
 *	ReadConsoleOutputA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ReadConsoleOutputA( HANDLE handle, CHAR_INFO *buffer, COORD size,
                                                  COORD coord, SMALL_RECT *region )
{
    BOOL ret;
    int y;

    ret = ReadConsoleOutputW( handle, buffer, size, coord, region );
    if (ret && region->Right >= region->Left)
    {
        UINT cp = GetConsoleOutputCP();
        for (y = 0; y <= region->Bottom - region->Top; y++)
            char_info_WtoA( cp, &buffer[(coord.Y + y) * size.X + coord.X], region->Right - region->Left + 1 );
    }
    return ret;
}


/******************************************************************************
 *	ReadConsoleOutputW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ReadConsoleOutputW( HANDLE handle, CHAR_INFO *buffer, COORD size,
                                                  COORD coord, SMALL_RECT *region )
{
    struct condrv_output_params params;
    unsigned int width, height, y;
    SMALL_RECT *result;
    DWORD count;
    BOOL ret;

    if (region->Left > region->Right || region->Top > region->Bottom)
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    if (size.X <= coord.X || size.Y <= coord.Y)
    {
        region->Right  = region->Left - 1;
        region->Bottom = region->Top - 1;
        SetLastError( ERROR_INVALID_FUNCTION );
        return FALSE;
    }
    width = min( region->Right - region->Left + 1, size.X - coord.X );
    height = min( region->Bottom - region->Top + 1, size.Y - coord.Y );
    region->Right = region->Left + width - 1;
    region->Bottom = region->Top + height - 1;

    count = sizeof(*result) + width * height * sizeof(*buffer);
    if (!(result = HeapAlloc( GetProcessHeap(), 0, count )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }

    params.mode  = CHAR_INFO_MODE_TEXTATTR;
    params.x     = region->Left;
    params.y     = region->Top;
    params.width = width;
    if ((ret = console_ioctl( handle, IOCTL_CONDRV_READ_OUTPUT, &params, sizeof(params), result, count, &count )) && count)
    {
        CHAR_INFO *char_info = (CHAR_INFO *)(result + 1);
        *region = *result;
        width  = region->Right - region->Left + 1;
        height = region->Bottom - region->Top + 1;
        for (y = 0; y < height; y++)
            memcpy( &buffer[(y + coord.Y) * size.X + coord.X], &char_info[y * width], width * sizeof(*buffer) );
    }
    HeapFree( GetProcessHeap(), 0, result );
    return ret;
}


/******************************************************************************
 *	ScrollConsoleScreenBufferA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ScrollConsoleScreenBufferA( HANDLE handle, const SMALL_RECT *scroll,
                                                          const SMALL_RECT *clip, COORD origin, const CHAR_INFO *fill )
{
    CHAR_INFO ciW;

    ciW.Attributes = fill->Attributes;
    MultiByteToWideChar( GetConsoleOutputCP(), 0, &fill->Char.AsciiChar, 1, &ciW.Char.UnicodeChar, 1 );

    return ScrollConsoleScreenBufferW( handle, scroll, clip, origin, &ciW );
}


/******************************************************************************
 *	ScrollConsoleScreenBufferW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ScrollConsoleScreenBufferW( HANDLE handle, const SMALL_RECT *scroll,
                                                          const SMALL_RECT *clip_rect, COORD origin,
                                                          const CHAR_INFO *fill )
{
    struct condrv_scroll_params params;

    if (clip_rect)
	TRACE( "(%p,(%d,%d-%d,%d),(%d,%d-%d,%d),%d-%d,%p)\n", handle,
               scroll->Left, scroll->Top, scroll->Right, scroll->Bottom,
               clip_rect->Left, clip_rect->Top, clip_rect->Right, clip_rect->Bottom,
               origin.X, origin.Y, fill );
    else
	TRACE("(%p,(%d,%d-%d,%d),(nil),%d-%d,%p)\n", handle,
	      scroll->Left, scroll->Top, scroll->Right, scroll->Bottom,
	      origin.X, origin.Y, fill );

    params.scroll    = *scroll;
    params.origin    = origin;
    params.fill.ch   = fill->Char.UnicodeChar;
    params.fill.attr = fill->Attributes;
    if (!clip_rect)
    {
        params.clip.Left = params.clip.Top = 0;
        params.clip.Right = params.clip.Bottom = SHRT_MAX;
    }
    else params.clip = *clip_rect;
    return console_ioctl( handle, IOCTL_CONDRV_SCROLL, (void *)&params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleActiveScreenBuffer   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleActiveScreenBuffer( HANDLE handle )
{
    TRACE( "(%p)\n", handle );
    return console_ioctl( handle, IOCTL_CONDRV_ACTIVATE, NULL, 0, NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleCP   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleCP( UINT cp )
{
    struct condrv_input_info_params params = { SET_CONSOLE_INPUT_INFO_INPUT_CODEPAGE };

    if (!IsValidCodePage( cp ))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    params.info.input_cp = cp;
    return console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle,
                          IOCTL_CONDRV_SET_INPUT_INFO, &params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleCtrlHandler   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleCtrlHandler( PHANDLER_ROUTINE func, BOOL add )
{
    struct ctrl_handler *handler;
    BOOL ret = FALSE;

    TRACE( "(%p,%d)\n", func, add );

    RtlEnterCriticalSection( &console_section );

    if (!func)
    {
        if (add) NtCurrentTeb()->Peb->ProcessParameters->ConsoleFlags |= 1;
        else NtCurrentTeb()->Peb->ProcessParameters->ConsoleFlags &= ~1;
        ret = TRUE;
    }
    else if (add)
    {
        if ((handler = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*handler) )))
        {
            handler->func = func;
            handler->next = ctrl_handlers;
            ctrl_handlers = handler;
            ret = TRUE;
        }
    }
    else
    {
        struct ctrl_handler **p_handler;

        for (p_handler = &ctrl_handlers; *p_handler; p_handler = &(*p_handler)->next)
        {
            if ((*p_handler)->func == func) break;
        }
        if (*p_handler && *p_handler != &default_handler)
        {
            handler = *p_handler;
            *p_handler = handler->next;
            RtlFreeHeap( GetProcessHeap(), 0, handler );
            ret = TRUE;
        }
        else SetLastError( ERROR_INVALID_PARAMETER );
    }

    RtlLeaveCriticalSection( &console_section );
    return ret;
}


/******************************************************************************
 *	SetConsoleCursorInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleCursorInfo( HANDLE handle, CONSOLE_CURSOR_INFO *info )
{
    struct condrv_output_info_params params = { SET_CONSOLE_OUTPUT_INFO_CURSOR_GEOM };

    TRACE( "(%p,%d,%d)\n", handle, info->dwSize, info->bVisible);

    params.info.cursor_size    = info->dwSize;
    params.info.cursor_visible = info->bVisible;
    return console_ioctl( handle, IOCTL_CONDRV_SET_OUTPUT_INFO, &params, sizeof(params),
                          NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleCursorPosition   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleCursorPosition( HANDLE handle, COORD pos )
{
    struct condrv_output_info_params params = { SET_CONSOLE_OUTPUT_INFO_CURSOR_POS };
    CONSOLE_SCREEN_BUFFER_INFO info;
    int w, h, do_move = 0;

    TRACE( "%p %d %d\n", handle, pos.X, pos.Y );

    params.info.cursor_x = pos.X;
    params.info.cursor_y = pos.Y;
    if (!console_ioctl( handle, IOCTL_CONDRV_SET_OUTPUT_INFO, &params, sizeof(params), NULL, 0, NULL ))
        return FALSE;

    if (!GetConsoleScreenBufferInfo( handle, &info )) return FALSE;

    /* if cursor is no longer visible, scroll the visible window... */
    w = info.srWindow.Right - info.srWindow.Left + 1;
    h = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (pos.X < info.srWindow.Left)
    {
	info.srWindow.Left = min(pos.X, info.dwSize.X - w);
	do_move = 1;
    }
    else if (pos.X > info.srWindow.Right)
    {
	info.srWindow.Left = max(pos.X, w) - w + 1;
	do_move = 1;
    }
    info.srWindow.Right = info.srWindow.Left + w - 1;

    if (pos.Y < info.srWindow.Top)
    {
	info.srWindow.Top = min(pos.Y, info.dwSize.Y - h);
	do_move = 1;
    }
    else if (pos.Y > info.srWindow.Bottom)
    {
	info.srWindow.Top = max(pos.Y, h) - h + 1;
	do_move = 1;
    }
    info.srWindow.Bottom = info.srWindow.Top + h - 1;

    return !do_move || SetConsoleWindowInfo( handle, TRUE, &info.srWindow );
}


/******************************************************************************
 *	SetConsoleInputExeNameA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleInputExeNameA( LPCSTR name )
{
    if (!name || !name[0])
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    RtlEnterCriticalSection( &console_section );
    MultiByteToWideChar( CP_ACP, 0, name, -1, input_exe, ARRAY_SIZE(input_exe) );
    RtlLeaveCriticalSection( &console_section );
    return TRUE;
}


/******************************************************************************
 *	SetConsoleInputExeNameW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleInputExeNameW( LPCWSTR name )
{
    if (!name || !name[0])
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    RtlEnterCriticalSection( &console_section );
    lstrcpynW( input_exe, name, ARRAY_SIZE(input_exe) );
    RtlLeaveCriticalSection( &console_section );
    return TRUE;
}


/******************************************************************************
 *	SetConsoleMode   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleMode( HANDLE handle, DWORD mode )
{
    TRACE( "(%p,%x)\n", handle, mode );
    return console_ioctl( handle, IOCTL_CONDRV_SET_MODE, &mode, sizeof(mode), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleOutputCP   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleOutputCP( UINT cp )
{
    struct condrv_input_info_params params = { SET_CONSOLE_INPUT_INFO_OUTPUT_CODEPAGE };

    if (!IsValidCodePage( cp ))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    params.info.output_cp = cp;
    return console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle,
                          IOCTL_CONDRV_SET_INPUT_INFO, &params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleScreenBufferInfoEx   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleScreenBufferInfoEx( HANDLE handle,
                                                            CONSOLE_SCREEN_BUFFER_INFOEX *info )
{
    struct condrv_output_info_params params =
        { SET_CONSOLE_OUTPUT_INFO_CURSOR_POS | SET_CONSOLE_OUTPUT_INFO_SIZE |
          SET_CONSOLE_OUTPUT_INFO_ATTR | SET_CONSOLE_OUTPUT_INFO_POPUP_ATTR |
          SET_CONSOLE_OUTPUT_INFO_DISPLAY_WINDOW | SET_CONSOLE_OUTPUT_INFO_MAX_SIZE };

    TRACE("(%p, %p)\n", handle, info);

    if (info->cbSize != sizeof(CONSOLE_SCREEN_BUFFER_INFOEX))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    params.info.width      = info->dwSize.X;
    params.info.height     = info->dwSize.Y;
    params.info.cursor_x   = info->dwCursorPosition.X;
    params.info.cursor_y   = info->dwCursorPosition.Y;
    params.info.attr       = info->wAttributes;
    params.info.win_left   = info->srWindow.Left;
    params.info.win_top    = info->srWindow.Top;
    params.info.win_right  = info->srWindow.Right;
    params.info.win_bottom = info->srWindow.Bottom;
    params.info.popup_attr = info->wPopupAttributes;
    params.info.max_width  = min( info->dwMaximumWindowSize.X, info->dwSize.X );
    params.info.max_height = min( info->dwMaximumWindowSize.Y, info->dwSize.Y );
    return console_ioctl( handle, IOCTL_CONDRV_SET_OUTPUT_INFO, &params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleScreenBufferSize   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleScreenBufferSize( HANDLE handle, COORD size )
{
    struct condrv_output_info_params params = { SET_CONSOLE_OUTPUT_INFO_SIZE };

    TRACE( "(%p,(%d,%d))\n", handle, size.X, size.Y );

    params.info.width  = size.X;
    params.info.height = size.Y;
    return console_ioctl( handle, IOCTL_CONDRV_SET_OUTPUT_INFO, &params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleTextAttribute   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleTextAttribute( HANDLE handle, WORD attr )
{
    struct condrv_output_info_params params = { SET_CONSOLE_OUTPUT_INFO_ATTR };

    TRACE( "(%p,%d)\n", handle, attr );

    params.info.attr = attr;
    return console_ioctl( handle, IOCTL_CONDRV_SET_OUTPUT_INFO, &params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleTitleW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleTitleW( LPCWSTR title )
{
    TRACE( "%s\n", debugstr_w( title ));

    return console_ioctl( RtlGetCurrentPeb()->ProcessParameters->ConsoleHandle, IOCTL_CONDRV_SET_TITLE,
                          (void *)title, lstrlenW(title) * sizeof(WCHAR), NULL, 0, NULL );
}


/******************************************************************************
 *	SetConsoleWindowInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetConsoleWindowInfo( HANDLE handle, BOOL absolute, SMALL_RECT *window )
{
    struct condrv_output_info_params params = { SET_CONSOLE_OUTPUT_INFO_DISPLAY_WINDOW };
    SMALL_RECT rect = *window;

    TRACE( "(%p,%d,(%d,%d-%d,%d))\n", handle, absolute, rect.Left, rect.Top, rect.Right, rect.Bottom );

    if (!absolute)
    {
	CONSOLE_SCREEN_BUFFER_INFO info;

	if (!GetConsoleScreenBufferInfo( handle, &info )) return FALSE;
	rect.Left   += info.srWindow.Left;
	rect.Top    += info.srWindow.Top;
	rect.Right  += info.srWindow.Right;
	rect.Bottom += info.srWindow.Bottom;
    }

    params.info.win_left   = rect.Left;
    params.info.win_top    = rect.Top;
    params.info.win_right  = rect.Right;
    params.info.win_bottom = rect.Bottom;
    return console_ioctl( handle, IOCTL_CONDRV_SET_OUTPUT_INFO, &params, sizeof(params), NULL, 0, NULL );
}


/******************************************************************************
 *	WriteConsoleInputA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleInputA( HANDLE handle, const INPUT_RECORD *buffer,
                                                  DWORD count, DWORD *written )
{
    INPUT_RECORD *recW = NULL;
    BOOL ret;

    if (count > 0)
    {
        if (!buffer)
        {
            SetLastError( ERROR_INVALID_ACCESS );
            return FALSE;
        }
        if (!(recW = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*recW) )))
        {
            SetLastError( ERROR_NOT_ENOUGH_MEMORY );
            return FALSE;
        }
        memcpy( recW, buffer, count * sizeof(*recW) );
        input_records_AtoW( recW, count );
    }
    ret = WriteConsoleInputW( handle, recW, count, written );
    HeapFree( GetProcessHeap(), 0, recW );
    return ret;
}


/******************************************************************************
 *	WriteConsoleInputW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleInputW( HANDLE handle, const INPUT_RECORD *buffer,
                                                  DWORD count, DWORD *written )
{
    TRACE( "(%p,%p,%d,%p)\n", handle, buffer, count, written );

    if (count > 0 && !buffer)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    if (!DeviceIoControl( handle, IOCTL_CONDRV_WRITE_INPUT, (void *)buffer, count * sizeof(*buffer), NULL, 0, NULL, NULL ))
        return FALSE;

    if (!written)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }
    *written = count;
    return TRUE;
}


/***********************************************************************
 *	WriteConsoleOutputA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleOutputA( HANDLE handle, const CHAR_INFO *buffer,
                                                   COORD size, COORD coord, SMALL_RECT *region )
{
    int y;
    BOOL ret;
    COORD new_size, new_coord;
    CHAR_INFO *ciW;

    new_size.X = min( region->Right - region->Left + 1, size.X - coord.X );
    new_size.Y = min( region->Bottom - region->Top + 1, size.Y - coord.Y );

    if (new_size.X <= 0 || new_size.Y <= 0)
    {
        region->Bottom = region->Top + new_size.Y - 1;
        region->Right = region->Left + new_size.X - 1;
        return TRUE;
    }

    /* only copy the useful rectangle */
    if (!(ciW = HeapAlloc( GetProcessHeap(), 0, sizeof(CHAR_INFO) * new_size.X * new_size.Y )))
        return FALSE;
    for (y = 0; y < new_size.Y; y++)
        memcpy( &ciW[y * new_size.X], &buffer[(y + coord.Y) * size.X + coord.X],
                new_size.X * sizeof(CHAR_INFO) );
    char_info_AtoW( ciW, new_size.X * new_size.Y );
    new_coord.X = new_coord.Y = 0;
    ret = WriteConsoleOutputW( handle, ciW, new_size, new_coord, region );
    HeapFree( GetProcessHeap(), 0, ciW );
    return ret;
}


/***********************************************************************
 *	WriteConsoleOutputW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleOutputW( HANDLE handle, const CHAR_INFO *buffer,
                                                   COORD size, COORD coord, SMALL_RECT *region )
{
    struct condrv_output_params *params;
    unsigned int width, height, y;
    size_t params_size;
    BOOL ret;

    TRACE( "(%p,%p,(%d,%d),(%d,%d),(%d,%dx%d,%d)\n",
           handle, buffer, size.X, size.Y, coord.X, coord.Y,
           region->Left, region->Top, region->Right, region->Bottom );

    if (region->Left > region->Right || region->Top > region->Bottom || size.X <= coord.X || size.Y <= coord.Y)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    width  = min( region->Right - region->Left + 1, size.X - coord.X );
    height = min( region->Bottom - region->Top + 1, size.Y - coord.Y );
    region->Right = region->Left + width - 1;
    region->Bottom = region->Top + height - 1;

    params_size = sizeof(*params) + width * height * sizeof(*buffer);
    if (!(params = HeapAlloc( GetProcessHeap(), 0, params_size )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }

    params->mode  = CHAR_INFO_MODE_TEXTATTR;
    params->x     = region->Left;
    params->y     = region->Top;
    params->width = width;

    for (y = 0; y < height; y++)
        memcpy( &((CHAR_INFO *)(params + 1))[y * width], &buffer[(y + coord.Y) * size.X + coord.X], width * sizeof(CHAR_INFO) );

    ret = console_ioctl( handle, IOCTL_CONDRV_WRITE_OUTPUT, params, params_size, region, sizeof(*region), NULL );
    HeapFree( GetProcessHeap(), 0, params );
    return ret;
}


/******************************************************************************
 *	WriteConsoleOutputAttribute   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleOutputAttribute( HANDLE handle, const WORD *attr, DWORD length,
                                                           COORD coord, DWORD *written )
{
    struct condrv_output_params *params;
    size_t size;
    BOOL ret;

    TRACE( "(%p,%p,%d,%dx%d,%p)\n", handle, attr, length, coord.X, coord.Y, written );

    if ((length > 0 && !attr) || !written)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    *written = 0;
    size = sizeof(*params) + length * sizeof(WORD);
    if (!(params = HeapAlloc( GetProcessHeap(), 0, size ))) return FALSE;
    params->mode   = CHAR_INFO_MODE_ATTR;
    params->x      = coord.X;
    params->y      = coord.Y;
    params->width  = 0;
    memcpy( params + 1, attr, length * sizeof(*attr) );
    ret = console_ioctl( handle, IOCTL_CONDRV_WRITE_OUTPUT, params, size, written, sizeof(*written), NULL );
    HeapFree( GetProcessHeap(), 0, params );
    return ret;
}


/******************************************************************************
 *	WriteConsoleOutputCharacterA   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleOutputCharacterA( HANDLE handle, LPCSTR str, DWORD length,
                                                            COORD coord, DWORD *written )
{
    BOOL ret;
    LPWSTR strW = NULL;
    DWORD lenW = 0;

    TRACE( "(%p,%s,%d,%dx%d,%p)\n", handle, debugstr_an(str, length), length, coord.X, coord.Y, written );

    if (length > 0)
    {
        UINT cp = GetConsoleOutputCP();
        if (!str)
        {
            SetLastError( ERROR_INVALID_ACCESS );
            return FALSE;
        }
        lenW = MultiByteToWideChar( cp, 0, str, length, NULL, 0 );

        if (!(strW = HeapAlloc( GetProcessHeap(), 0, lenW * sizeof(WCHAR) )))
        {
            SetLastError( ERROR_NOT_ENOUGH_MEMORY );
            return FALSE;
        }
        MultiByteToWideChar( cp, 0, str, length, strW, lenW );
    }
    ret = WriteConsoleOutputCharacterW( handle, strW, lenW, coord, written );
    HeapFree( GetProcessHeap(), 0, strW );
    return ret;
}


/******************************************************************************
 *	WriteConsoleOutputCharacterW   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH WriteConsoleOutputCharacterW( HANDLE handle, LPCWSTR str, DWORD length,
                                                            COORD coord, DWORD *written )
{
    struct condrv_output_params *params;
    size_t size;
    BOOL ret;

    TRACE( "(%p,%s,%d,%dx%d,%p)\n", handle, debugstr_wn(str, length), length, coord.X, coord.Y, written );

    if ((length > 0 && !str) || !written)
    {
        SetLastError( ERROR_INVALID_ACCESS );
        return FALSE;
    }

    *written = 0;
    size = sizeof(*params) + length * sizeof(WCHAR);
    if (!(params = HeapAlloc( GetProcessHeap(), 0, size ))) return FALSE;
    params->mode   = CHAR_INFO_MODE_TEXT;
    params->x      = coord.X;
    params->y      = coord.Y;
    params->width  = 0;
    memcpy( params + 1, str, length * sizeof(*str) );
    ret = console_ioctl( handle, IOCTL_CONDRV_WRITE_OUTPUT, params, size, written, sizeof(*written), NULL );
    HeapFree( GetProcessHeap(), 0, params );
    return ret;
}

static HANDLE create_pseudo_console( COORD size, HANDLE input, HANDLE output, HANDLE signal,
                                     DWORD flags, HANDLE *process )
{
    WCHAR cmd[MAX_PATH], conhost_path[MAX_PATH];
    PROCESS_INFORMATION pi;
    HANDLE server, console;
    STARTUPINFOEXW si;
    void *redir;
    BOOL res;

    if (!(server = create_console_server())) return NULL;

    console = create_console_reference( server );
    if (!console)
    {
        NtClose( server );
        return NULL;
    }

    memset( &si, 0, sizeof(si) );
    si.StartupInfo.cb         = sizeof(STARTUPINFOEXW);
    si.StartupInfo.hStdInput  = input;
    si.StartupInfo.hStdOutput = output;
    si.StartupInfo.hStdError  = output;
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    swprintf( conhost_path, ARRAY_SIZE(conhost_path), L"%s\\conhost.exe", system_dir );
    swprintf( cmd, ARRAY_SIZE(cmd),
              L"\"%s\" --headless %s--width %u --height %u --signal 0x%x --server 0x%x",
              conhost_path, (flags & PSEUDOCONSOLE_INHERIT_CURSOR) ? L"--inheritcursor " : L"",
              size.X, size.Y, signal, server );

    Wow64DisableWow64FsRedirection( &redir );
    res = CreateProcessW( conhost_path, cmd, NULL, NULL, TRUE, DETACHED_PROCESS, NULL, NULL,
                          &si.StartupInfo, &pi );
    Wow64RevertWow64FsRedirection( redir );
    NtClose( server );
    if (!res)
    {
        NtClose( console );
        return NULL;
    }

    NtClose( pi.hThread );
    *process = pi.hProcess;
    return console;
}

/******************************************************************************
 *	CreatePseudoConsole   (kernelbase.@)
 */
HRESULT WINAPI CreatePseudoConsole( COORD size, HANDLE input, HANDLE output, DWORD flags, HPCON *ret )
{
    SECURITY_ATTRIBUTES inherit_attr = { sizeof(inherit_attr), NULL, TRUE };
    struct pseudo_console *pseudo_console;
    HANDLE signal = NULL;
    WCHAR pipe_name[64];

    TRACE( "(%u,%u) %p %p %x %p\n", size.X, size.Y, input, output, flags, ret );

    if (!size.X || !size.Y || !ret) return E_INVALIDARG;

    if (!(pseudo_console = HeapAlloc( GetProcessHeap(), 0, HEAP_ZERO_MEMORY ))) return E_OUTOFMEMORY;

    swprintf( pipe_name, ARRAY_SIZE(pipe_name),  L"\\\\.\\pipe\\wine_pty_signal_pipe%x",
              GetCurrentThreadId() );
    signal = CreateNamedPipeW( pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE,
                               PIPE_UNLIMITED_INSTANCES, 4096, 4096, NMPWAIT_USE_DEFAULT_WAIT, &inherit_attr );
    if (signal == INVALID_HANDLE_VALUE)
    {
        HeapFree( GetProcessHeap(), 0, pseudo_console );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    pseudo_console->signal = CreateFileW( pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
    if (pseudo_console->signal != INVALID_HANDLE_VALUE)
        pseudo_console->reference = create_pseudo_console( size, input, output, signal, flags,
                                                           &pseudo_console->process );
    NtClose( signal );
    if (!pseudo_console->reference)
    {
        ClosePseudoConsole( pseudo_console );
        return HRESULT_FROM_WIN32( GetLastError() );
    }

    *ret = pseudo_console;
    return S_OK;
}

/******************************************************************************
 *	ClosePseudoConsole   (kernelbase.@)
 */
void WINAPI ClosePseudoConsole( HPCON handle )
{
    struct pseudo_console *pseudo_console = handle;

    TRACE( "%p\n", handle );

    if (!pseudo_console) return;
    if (pseudo_console->signal) CloseHandle( pseudo_console->signal );
    if (pseudo_console->process)
    {
        WaitForSingleObject( pseudo_console->process, INFINITE );
        CloseHandle( pseudo_console->process );
    }
    if (pseudo_console->reference) CloseHandle( pseudo_console->reference );
}

/******************************************************************************
 *	ResizePseudoConsole   (kernelbase.@)
 */
HRESULT WINAPI ResizePseudoConsole( HPCON handle, COORD size )
{
    FIXME( "%p (%u,%u)\n", handle, size.X, size.Y );
    return E_NOTIMPL;
}

void init_console( void )
{
    RTL_USER_PROCESS_PARAMETERS *params = RtlGetCurrentPeb()->ProcessParameters;

    if (params->ConsoleHandle == CONSOLE_HANDLE_ALLOC)
    {
        HMODULE mod = GetModuleHandleW( NULL );
        params->ConsoleHandle = NULL;
        if (RtlImageNtHeader( mod )->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI)
            AllocConsole();
    }
    else if (params->ConsoleHandle) create_console_connection( params->ConsoleHandle );

    RtlAddVectoredExceptionHandler( FALSE, handle_ctrl_c );
}
