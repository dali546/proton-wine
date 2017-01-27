/*
 * MimeOle tests
 *
 * Copyright 2007 Huw Davies
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

#define COBJMACROS
#define NONAMELESSUNION

#include "initguid.h"
#include "windows.h"
#include "ole2.h"
#include "ocidl.h"

#include "mimeole.h"

#include <stdio.h>

#include "wine/test.h"

#define DEFINE_EXPECT(func) \
    static BOOL expect_ ## func = FALSE, called_ ## func = FALSE

#define SET_EXPECT(func) \
    expect_ ## func = TRUE

#define CHECK_EXPECT(func) \
    do { \
        ok(expect_ ##func, "unexpected call " #func "\n"); \
        expect_ ## func = FALSE; \
        called_ ## func = TRUE; \
    }while(0)

#define CHECK_EXPECT2(func) \
    do { \
        ok(expect_ ##func, "unexpected call " #func  "\n"); \
        called_ ## func = TRUE; \
    }while(0)

#define CHECK_CALLED(func) \
    do { \
        ok(called_ ## func, "expected " #func "\n"); \
        expect_ ## func = called_ ## func = FALSE; \
    }while(0)

DEFINE_EXPECT(Stream_Read);
DEFINE_EXPECT(Stream_Stat);
DEFINE_EXPECT(Stream_Seek);
DEFINE_EXPECT(Stream_Seek_END);

static const char msg1[] =
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed;\r\n"
    " boundary=\"------------1.5.0.6\";\r\n"
    " stuff=\"du;nno\";\r\n"
    " morestuff=\"so\\\\me\\\"thing\\\"\"\r\n"
    "foo: bar\r\n"
    "From: Huw Davies <huw@codeweavers.com>\r\n"
    "From: Me <xxx@codeweavers.com>\r\n"
    "To: wine-patches <wine-patches@winehq.org>\r\n"
    "Cc: Huw Davies <huw@codeweavers.com>,\r\n"
    "    \"Fred Bloggs\"   <fred@bloggs.com>\r\n"
    "foo: baz\r\n"
    "bar: fum\r\n"
    "\r\n"
    "This is a multi-part message in MIME format.\r\n"
    "--------------1.5.0.6\r\n"
    "Content-Type: text/plain; format=fixed; charset=UTF-8\r\n"
    "Content-Transfer-Encoding: 8bit\r\n"
    "\r\n"
    "Stuff\r\n"
    "--------------1.5.0.6\r\n"
    "Content-Type: text/plain; charset=\"us-ascii\"\r\n"
    "Content-Transfer-Encoding: 7bit\r\n"
    "\r\n"
    "More stuff\r\n"
    "--------------1.5.0.6--\r\n";

static void test_CreateVirtualStream(void)
{
    HRESULT hr;
    IStream *pstm;

    hr = MimeOleCreateVirtualStream(&pstm);
    ok(hr == S_OK, "ret %08x\n", hr);

    IStream_Release(pstm);
}

static void test_CreateSecurity(void)
{
    HRESULT hr;
    IMimeSecurity *sec;

    hr = MimeOleCreateSecurity(&sec);
    ok(hr == S_OK, "ret %08x\n", hr);

    IMimeSecurity_Release(sec);
}

static IStream *create_stream_from_string(const char *data)
{
    LARGE_INTEGER off;
    IStream *stream;
    HRESULT hr;

    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IStream_Write(stream, data, strlen(data), NULL);
    ok(hr == S_OK, "Write failed: %08x\n", hr);

    off.QuadPart = 0;
    hr = IStream_Seek(stream, off, STREAM_SEEK_SET, NULL);
    ok(hr == S_OK, "Seek failed: %08x\n", hr);

    return stream;
}

#define test_current_encoding(a,b) _test_current_encoding(__LINE__,a,b)
static void _test_current_encoding(unsigned line, IMimeBody *mime_body, ENCODINGTYPE encoding)
{
    ENCODINGTYPE current_encoding;
    HRESULT hres;

    hres = IMimeBody_GetCurrentEncoding(mime_body, &current_encoding);
    ok_(__FILE__,line)(hres == S_OK, "GetCurrentEncoding failed: %08x\n", hres);
    ok_(__FILE__,line)(current_encoding == encoding, "encoding = %d, expected %d\n", current_encoding, encoding);
}

static void test_CreateBody(void)
{
    HRESULT hr;
    IMimeBody *body;
    HBODY handle = (void *)0xdeadbeef;
    IStream *in;
    LARGE_INTEGER off;
    ULARGE_INTEGER pos;
    ENCODINGTYPE enc;
    ULONG count, found_param, i;
    MIMEPARAMINFO *param_info;
    IMimeAllocator *alloc;
    BODYOFFSETS offsets;

    hr = CoCreateInstance(&CLSID_IMimeBody, NULL, CLSCTX_INPROC_SERVER, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetHandle(body, &handle);
    ok(hr == MIME_E_NO_DATA, "ret %08x\n", hr);
    ok(handle == NULL, "handle %p\n", handle);

    in = create_stream_from_string(msg1);

    /* Need to call InitNew before Load otherwise Load crashes with native inetcomm */
    hr = IMimeBody_InitNew(body);
    ok(hr == S_OK, "ret %08x\n", hr);

    test_current_encoding(body, IET_7BIT);

    hr = IMimeBody_Load(body, in);
    ok(hr == S_OK, "ret %08x\n", hr);
    off.QuadPart = 0;
    IStream_Seek(in, off, STREAM_SEEK_CUR, &pos);
    ok(pos.u.LowPart == 359, "pos %u\n", pos.u.LowPart);

    hr = IMimeBody_IsContentType(body, "multipart", "mixed");
    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeBody_IsContentType(body, "text", "plain");
    ok(hr == S_FALSE, "ret %08x\n", hr);
    hr = IMimeBody_IsContentType(body, NULL, "mixed");
    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeBody_IsType(body, IBT_EMPTY);
    ok(hr == S_OK, "got %08x\n", hr);

    hr = IMimeBody_SetData(body, IET_8BIT, "text", "plain", &IID_IStream, in);
    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeBody_IsContentType(body, "text", "plain");
    todo_wine
        ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeBody_GetCurrentEncoding(body, &enc);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(enc == IET_8BIT, "encoding %d\n", enc);

    memset(&offsets, 0xcc, sizeof(offsets));
    hr = IMimeBody_GetOffsets(body, &offsets);
    ok(hr == MIME_E_NO_DATA, "ret %08x\n", hr);
    ok(offsets.cbBoundaryStart == 0, "got %d\n", offsets.cbBoundaryStart);
    ok(offsets.cbHeaderStart == 0, "got %d\n", offsets.cbHeaderStart);
    ok(offsets.cbBodyStart == 0, "got %d\n", offsets.cbBodyStart);
    ok(offsets.cbBodyEnd == 0, "got %d\n", offsets.cbBodyEnd);

    hr = IMimeBody_IsType(body, IBT_EMPTY);
    ok(hr == S_FALSE, "got %08x\n", hr);

    hr = MimeOleGetAllocator(&alloc);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetParameters(body, "nothere", &count, &param_info);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);
    ok(count == 0, "got %d\n", count);
    ok(!param_info, "got %p\n", param_info);

    hr = IMimeBody_GetParameters(body, "bar", &count, &param_info);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(count == 0, "got %d\n", count);
    ok(!param_info, "got %p\n", param_info);

    hr = IMimeBody_GetParameters(body, "Content-Type", &count, &param_info);
    ok(hr == S_OK, "ret %08x\n", hr);
    todo_wine  /* native adds a charset parameter */
        ok(count == 4, "got %d\n", count);
    ok(param_info != NULL, "got %p\n", param_info);

    found_param = 0;
    for(i = 0; i < count; i++)
    {
        if(!strcmp(param_info[i].pszName, "morestuff"))
        {
            found_param++;
            ok(!strcmp(param_info[i].pszData, "so\\me\"thing\""),
               "got %s\n", param_info[i].pszData);
        }
        else if(!strcmp(param_info[i].pszName, "stuff"))
        {
            found_param++;
            ok(!strcmp(param_info[i].pszData, "du;nno"),
               "got %s\n", param_info[i].pszData);
        }
    }
    ok(found_param == 2, "matched %d params\n", found_param);

    hr = IMimeAllocator_FreeParamInfoArray(alloc, count, param_info, TRUE);
    ok(hr == S_OK, "ret %08x\n", hr);
    IMimeAllocator_Release(alloc);

    IStream_Release(in);
    IMimeBody_Release(body);
}

typedef struct {
    IStream IStream_iface;
    LONG ref;
    unsigned pos;
} TestStream;

static inline TestStream *impl_from_IStream(IStream *iface)
{
    return CONTAINING_RECORD(iface, TestStream, IStream_iface);
}

static HRESULT WINAPI Stream_QueryInterface(IStream *iface, REFIID riid, void **ppv)
{
    if(IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_ISequentialStream, riid) || IsEqualGUID(&IID_IStream, riid)) {
        *ppv = iface;
        return S_OK;
    }

    ok(0, "unexpected call %s\n", wine_dbgstr_guid(riid));
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Stream_AddRef(IStream *iface)
{
    TestStream *This = impl_from_IStream(iface);
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI Stream_Release(IStream *iface)
{
    TestStream *This = impl_from_IStream(iface);
    return InterlockedDecrement(&This->ref);
}

static HRESULT WINAPI Stream_Read(IStream *iface, void *pv, ULONG cb, ULONG *pcbRead)
{
    TestStream *This = impl_from_IStream(iface);
    BYTE *output = pv;
    unsigned i;

    CHECK_EXPECT(Stream_Read);

    for(i = 0; i < cb; i++)
        output[i] = '0' + This->pos++;
    *pcbRead = i;
    return S_OK;
}

static HRESULT WINAPI Stream_Write(IStream *iface, const void *pv, ULONG cb, ULONG *pcbWritten)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static DWORD expect_seek_pos;

static HRESULT WINAPI Stream_Seek(IStream *iface, LARGE_INTEGER dlibMove, DWORD dwOrigin,
                                  ULARGE_INTEGER *plibNewPosition)
{
    TestStream *This = impl_from_IStream(iface);

    if(dwOrigin == STREAM_SEEK_END) {
        CHECK_EXPECT(Stream_Seek_END);
        ok(dlibMove.QuadPart == expect_seek_pos, "unexpected seek pos %u\n", dlibMove.u.LowPart);
        if(plibNewPosition)
            plibNewPosition->QuadPart = 10;
        return S_OK;
    }

    CHECK_EXPECT(Stream_Seek);

    ok(dlibMove.QuadPart == expect_seek_pos, "unexpected seek pos %u\n", dlibMove.u.LowPart);
    ok(dwOrigin == STREAM_SEEK_SET, "dwOrigin = %d\n", dwOrigin);
    This->pos = dlibMove.QuadPart;
    if(plibNewPosition)
        plibNewPosition->QuadPart = This->pos;
    return S_OK;
}

static HRESULT WINAPI Stream_SetSize(IStream *iface, ULARGE_INTEGER libNewSize)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_CopyTo(IStream *iface, IStream *pstm, ULARGE_INTEGER cb,
                                    ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_Commit(IStream *iface, DWORD grfCommitFlags)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_Revert(IStream *iface)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_LockRegion(IStream *iface, ULARGE_INTEGER libOffset,
                                        ULARGE_INTEGER cb, DWORD dwLockType)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_UnlockRegion(IStream *iface,
        ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_Stat(IStream *iface, STATSTG *pstatstg, DWORD dwStatFlag)
{
    CHECK_EXPECT(Stream_Stat);
    ok(dwStatFlag == STATFLAG_NONAME, "dwStatFlag = %x\n", dwStatFlag);
    return E_NOTIMPL;
}

static HRESULT WINAPI Stream_Clone(IStream *iface, IStream **ppstm)
{
    ok(0, "unexpected call\n");
    return E_NOTIMPL;
}

static const IStreamVtbl StreamVtbl = {
    Stream_QueryInterface,
    Stream_AddRef,
    Stream_Release,
    Stream_Read,
    Stream_Write,
    Stream_Seek,
    Stream_SetSize,
    Stream_CopyTo,
    Stream_Commit,
    Stream_Revert,
    Stream_LockRegion,
    Stream_UnlockRegion,
    Stream_Stat,
    Stream_Clone
};

static TestStream *create_test_stream(void)
{
    TestStream *stream;
    stream = HeapAlloc(GetProcessHeap(), 0, sizeof(*stream));
    stream->IStream_iface.lpVtbl = &StreamVtbl;
    stream->ref = 1;
    stream->pos = 0;
    return stream;
}

#define test_stream_read(a,b,c,d) _test_stream_read(__LINE__,a,b,c,d)
static void _test_stream_read(unsigned line, IStream *stream, HRESULT exhres, const char *exdata, unsigned read_size)
{
    ULONG read = 0xdeadbeed, exread = strlen(exdata);
    char buf[1024];
    HRESULT hres;

    if(read_size == -1)
        read_size = sizeof(buf)-1;

    hres = IStream_Read(stream, buf, read_size, &read);
    ok_(__FILE__,line)(hres == exhres, "Read returned %08x, expected %08x\n", hres, exhres);
    ok_(__FILE__,line)(read == exread, "unexpected read size %u, expected %u\n", read, exread);
    buf[read] = 0;
    ok_(__FILE__,line)(read == exread && !memcmp(buf, exdata, read), "unexpected data %s\n", buf);
}

static void test_SetData(void)
{
    IStream *stream, *stream2;
    TestStream *test_stream;
    IMimeBody *body;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_IMimeBody, NULL, CLSCTX_INPROC_SERVER, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    /* Need to call InitNew before Load otherwise Load crashes with native inetcomm */
    hr = IMimeBody_InitNew(body);
    ok(hr == S_OK, "ret %08x\n", hr);

    stream = create_stream_from_string(msg1);
    hr = IMimeBody_Load(body, stream);
    ok(hr == S_OK, "ret %08x\n", hr);
    IStream_Release(stream);

    test_stream = create_test_stream();
    hr = IMimeBody_SetData(body, IET_BINARY, "text", "plain", &IID_IStream, &test_stream->IStream_iface);

    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeBody_IsContentType(body, "text", "plain");
    todo_wine
    ok(hr == S_OK, "ret %08x\n", hr);

    test_current_encoding(body, IET_BINARY);

    SET_EXPECT(Stream_Stat);
    SET_EXPECT(Stream_Seek_END);
    hr = IMimeBody_GetData(body, IET_BINARY, &stream);
    CHECK_CALLED(Stream_Stat);
    CHECK_CALLED(Stream_Seek_END);
    ok(hr == S_OK, "GetData failed %08x\n", hr);
    ok(stream != &test_stream->IStream_iface, "unexpected stream\n");

    SET_EXPECT(Stream_Seek);
    SET_EXPECT(Stream_Read);
    test_stream_read(stream, S_OK, "012", 3);
    CHECK_CALLED(Stream_Seek);
    CHECK_CALLED(Stream_Read);

    SET_EXPECT(Stream_Stat);
    SET_EXPECT(Stream_Seek_END);
    hr = IMimeBody_GetData(body, IET_BINARY, &stream2);
    CHECK_CALLED(Stream_Stat);
    CHECK_CALLED(Stream_Seek_END);
    ok(hr == S_OK, "GetData failed %08x\n", hr);
    ok(stream2 != stream, "unexpected stream\n");

    SET_EXPECT(Stream_Seek);
    SET_EXPECT(Stream_Read);
    test_stream_read(stream2, S_OK, "01", 2);
    CHECK_CALLED(Stream_Seek);
    CHECK_CALLED(Stream_Read);

    expect_seek_pos = 3;
    SET_EXPECT(Stream_Seek);
    SET_EXPECT(Stream_Read);
    test_stream_read(stream, S_OK, "345", 3);
    CHECK_CALLED(Stream_Seek);
    CHECK_CALLED(Stream_Read);

    IStream_Release(stream);
    IStream_Release(stream2);
    IStream_Release(&test_stream->IStream_iface);

    stream = create_stream_from_string(" \t\r\n|}~YWJj ZGV|}~mZw== \t"); /* "abcdefg" in base64 obscured by invalid chars */
    hr = IMimeBody_SetData(body, IET_BASE64, "text", "plain", &IID_IStream, stream);
    IStream_Release(stream);
    ok(hr == S_OK, "SetData failed: %08x\n", hr);

    test_current_encoding(body, IET_BASE64);

    hr = IMimeBody_GetData(body, IET_BINARY, &stream);
    ok(hr == S_OK, "GetData failed %08x\n", hr);

    test_stream_read(stream, S_OK, "abc", 3);
    test_stream_read(stream, S_OK, "defg", -1);

    IStream_Release(stream);

    hr = IMimeBody_GetData(body, IET_BASE64, &stream);
    ok(hr == S_OK, "GetData failed %08x\n", hr);

    test_stream_read(stream, S_OK, " \t\r", 3);
    IStream_Release(stream);

    IMimeBody_Release(body);
}

static void test_Allocator(void)
{
    HRESULT hr;
    IMimeAllocator *alloc;

    hr = MimeOleGetAllocator(&alloc);
    ok(hr == S_OK, "ret %08x\n", hr);
    IMimeAllocator_Release(alloc);
}

static void test_CreateMessage(void)
{
    HRESULT hr;
    IMimeMessage *msg;
    IStream *stream;
    LONG ref;
    HBODY hbody, hbody2;
    IMimeBody *body;
    BODYOFFSETS offsets;
    ULONG count;
    FINDBODY find_struct;
    HCHARSET hcs;
    HBODY handle = NULL;

    char text[] = "text";
    HBODY *body_list;
    PROPVARIANT prop;
    static const char att_pritype[] = "att:pri-content-type";

    hr = MimeOleCreateMessage(NULL, &msg);
    ok(hr == S_OK, "ret %08x\n", hr);

    stream = create_stream_from_string(msg1);

    hr = IMimeMessage_Load(msg, stream);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeMessage_CountBodies(msg, HBODY_ROOT, TRUE, &count);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(count == 3, "got %d\n", count);

    hr = IMimeMessage_CountBodies(msg, HBODY_ROOT, FALSE, &count);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(count == 3, "got %d\n", count);

    hr = IMimeMessage_BindToObject(msg, HBODY_ROOT, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeBody_GetOffsets(body, &offsets);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(offsets.cbBoundaryStart == 0, "got %d\n", offsets.cbBoundaryStart);
    ok(offsets.cbHeaderStart == 0, "got %d\n", offsets.cbHeaderStart);
    ok(offsets.cbBodyStart == 359, "got %d\n", offsets.cbBodyStart);
    ok(offsets.cbBodyEnd == 666, "got %d\n", offsets.cbBodyEnd);
    IMimeBody_Release(body);

    hr = IMimeMessage_GetBody(msg, IBL_ROOT, NULL, &hbody);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetHandle(body, NULL);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    hr = IMimeBody_GetHandle(body, &handle);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(handle != NULL, "handle %p\n", handle);

    hr = IMimeMessage_GetBody(msg, IBL_PARENT, hbody, NULL);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    hbody2 = (HBODY)0xdeadbeef;
    hr = IMimeMessage_GetBody(msg, IBL_PARENT, hbody, &hbody2);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);
    ok(hbody2 == NULL, "hbody2 %p\n", hbody2);

    PropVariantInit(&prop);
    hr = IMimeMessage_GetBodyProp(msg, hbody, att_pritype, 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(prop.vt == VT_LPSTR, "vt %08x\n", prop.vt);
    ok(!strcasecmp(prop.u.pszVal, "multipart"), "got %s\n", prop.u.pszVal);
    PropVariantClear(&prop);

    hr = IMimeMessage_GetBody(msg, IBL_FIRST, hbody, &hbody);
    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeMessage_BindToObject(msg, hbody, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetHandle(body, &handle);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(handle == hbody, "handle %p\n", handle);

    hr = IMimeBody_GetOffsets(body, &offsets);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(offsets.cbBoundaryStart == 405, "got %d\n", offsets.cbBoundaryStart);
    ok(offsets.cbHeaderStart == 428, "got %d\n", offsets.cbHeaderStart);
    ok(offsets.cbBodyStart == 518, "got %d\n", offsets.cbBodyStart);
    ok(offsets.cbBodyEnd == 523, "got %d\n", offsets.cbBodyEnd);

    hr = IMimeBody_GetCharset(body, &hcs);
    ok(hr == S_OK, "ret %08x\n", hr);
    todo_wine
    {
        ok(hcs != NULL, "Expected non-NULL charset\n");
    }

    IMimeBody_Release(body);

    hr = IMimeMessage_GetBody(msg, IBL_NEXT, hbody, &hbody);
    ok(hr == S_OK, "ret %08x\n", hr);
    hr = IMimeMessage_BindToObject(msg, hbody, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetHandle(body, &handle);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(handle == hbody, "handle %p\n", handle);

    hr = IMimeBody_GetOffsets(body, &offsets);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(offsets.cbBoundaryStart == 525, "got %d\n", offsets.cbBoundaryStart);
    ok(offsets.cbHeaderStart == 548, "got %d\n", offsets.cbHeaderStart);
    ok(offsets.cbBodyStart == 629, "got %d\n", offsets.cbBodyStart);
    ok(offsets.cbBodyEnd == 639, "got %d\n", offsets.cbBodyEnd);
    IMimeBody_Release(body);

    find_struct.pszPriType = text;
    find_struct.pszSubType = NULL;

    hr = IMimeMessage_FindFirst(msg, &find_struct, &hbody);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeMessage_FindNext(msg, &find_struct, &hbody);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeMessage_FindNext(msg, &find_struct, &hbody);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    hr = IMimeMessage_GetAttachments(msg, &count, &body_list);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(count == 2, "got %d\n", count);
    if(count == 2)
    {
        ENCODINGTYPE encoding;
        IMimeBody *attachment;
        PROPVARIANT prop;

        PropVariantInit(&prop);

        hr = IMimeMessage_BindToObject(msg, body_list[0], &IID_IMimeBody, (void**)&attachment);
        ok(hr == S_OK, "ret %08x\n", hr);

        hr = IMimeBody_IsContentType(attachment, "multipart", NULL);
        ok(hr == S_FALSE, "ret %08x\n", hr);

        hr = IMimeBody_GetCurrentEncoding(attachment, &encoding);
        ok(hr == S_OK, "ret %08x\n", hr);
        todo_wine ok(encoding == IET_8BIT, "ret %d\n", encoding);

        prop.vt = VT_LPSTR;
        hr = IMimeBody_GetProp(attachment, "Content-Transfer-Encoding", 0, &prop);
        ok(hr == S_OK, "ret %08x\n", hr);

        ok(prop.vt == VT_LPSTR, "type %d\n", prop.vt);
        ok(!strcmp(prop.u.pszVal, "8bit"), "got  %s\n", prop.u.pszVal);
        PropVariantClear(&prop);

        hr = IMimeBody_IsType(attachment, IBT_ATTACHMENT);
        todo_wine ok(hr == S_FALSE, "ret %08x\n", hr);

        IMimeBody_Release(attachment);

        hr = IMimeMessage_BindToObject(msg, body_list[1], &IID_IMimeBody, (void**)&attachment);
        ok(hr == S_OK, "ret %08x\n", hr);

        hr = IMimeBody_IsContentType(attachment, "multipart", NULL);
        ok(hr == S_FALSE, "ret %08x\n", hr);

        hr = IMimeBody_GetCurrentEncoding(attachment, &encoding);
        ok(hr == S_OK, "ret %08x\n", hr);
        todo_wine ok(encoding == IET_7BIT, "ret %d\n", encoding);

        prop.vt = VT_LPSTR;
        hr = IMimeBody_GetProp(attachment, "Content-Transfer-Encoding", 0, &prop);
        ok(hr == S_OK, "ret %08x\n", hr);
        ok(prop.vt == VT_LPSTR, "type %d\n", prop.vt);
        ok(!strcmp(prop.u.pszVal, "7bit"), "got  %s\n", prop.u.pszVal);
        PropVariantClear(&prop);

        hr = IMimeBody_IsType(attachment, IBT_ATTACHMENT);
        ok(hr == S_OK, "ret %08x\n", hr);

        IMimeBody_Release(attachment);
    }
    CoTaskMemFree(body_list);

    hr = IMimeBody_GetCharset(body, &hcs);
    ok(hr == S_OK, "ret %08x\n", hr);
    todo_wine
    {
        ok(hcs != NULL, "Expected non-NULL charset\n");
    }

    IMimeMessage_Release(msg);

    ref = IStream_AddRef(stream);
    ok(ref == 2 ||
       broken(ref == 1), /* win95 */
       "ref %d\n", ref);
    IStream_Release(stream);

    IStream_Release(stream);
}

static void test_MessageSetProp(void)
{
    static const char topic[] = "wine topic";
    static const WCHAR topicW[] = {'w','i','n','e',' ','t','o','p','i','c',0};
    HRESULT hr;
    IMimeMessage *msg;
    IMimeBody *body;
    PROPVARIANT prop;

    hr = MimeOleCreateMessage(NULL, &msg);
    ok(hr == S_OK, "ret %08x\n", hr);

    PropVariantInit(&prop);

    hr = IMimeMessage_BindToObject(msg, HBODY_ROOT, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_SetProp(body, NULL, 0, &prop);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    hr = IMimeBody_SetProp(body, "Thread-Topic", 0, NULL);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, "Thread-Topic", 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    hr = IMimeBody_GetProp(body, NULL, 0, &prop);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    hr = IMimeBody_GetProp(body, "Thread-Topic", 0, NULL);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    hr = IMimeBody_GetProp(body, "Wine-Topic", 0, &prop);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    prop.vt = VT_LPSTR;
    hr = IMimeBody_GetProp(body, "Thread-Topic", 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
        ok(prop.vt == VT_LPSTR, "type %d\n", prop.vt);
        ok(!strcmp(prop.u.pszVal, topic), "got  %s\n", prop.u.pszVal);
        PropVariantClear(&prop);
    }

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, PIDTOSTR(PID_HDR_SUBJECT), 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    prop.vt = VT_LPSTR;
    hr = IMimeBody_GetProp(body, PIDTOSTR(PID_HDR_SUBJECT), 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
        ok(prop.vt == VT_LPSTR, "type %d\n", prop.vt);
        ok(!strcmp(prop.u.pszVal, topic), "got  %s\n", prop.u.pszVal);
        PropVariantClear(&prop);
    }

    /* Using the name or PID returns the same result. */
    prop.vt = VT_LPSTR;
    hr = IMimeBody_GetProp(body, "Subject", 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
        ok(prop.vt == VT_LPSTR, "type %d\n", prop.vt);
        ok(!strcmp(prop.u.pszVal, topic), "got  %s\n", prop.u.pszVal);
        PropVariantClear(&prop);
    }

    prop.vt = VT_LPWSTR;
    hr = IMimeBody_GetProp(body, "Subject", 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
        ok(prop.vt == VT_LPWSTR, "type %d\n", prop.vt);
        ok(!lstrcmpW(prop.u.pwszVal, topicW), "got %s\n", wine_dbgstr_w(prop.u.pwszVal));
        PropVariantClear(&prop);
    }

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, PIDTOSTR(PID_HDR_TO), 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    /* Out of Range PID */
    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, PIDTOSTR(124), 0, &prop);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);
    PropVariantClear(&prop);

    IMimeBody_Release(body);
    IMimeMessage_Release(msg);
}

static void test_MessageGetPropInfo(void)
{
    static const char topic[] = "wine topic";
    static const char subject[] = "wine testing";
    HRESULT hr;
    IMimeMessage *msg;
    IMimeBody *body;
    PROPVARIANT prop;
    MIMEPROPINFO info;

    hr = MimeOleCreateMessage(NULL, &msg);
    ok(hr == S_OK, "ret %08x\n", hr);

    PropVariantInit(&prop);

    hr = IMimeMessage_BindToObject(msg, HBODY_ROOT, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, "Thread-Topic", 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(subject)+1);
    strcpy(prop.u.pszVal, subject);
    hr = IMimeBody_SetProp(body, PIDTOSTR(PID_HDR_SUBJECT), 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    memset(&info, 0, sizeof(info));
    info.dwMask = PIM_ENCODINGTYPE | PIM_FLAGS | PIM_PROPID;
    hr = IMimeBody_GetPropInfo(body, NULL, &info);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    memset(&info, 0, sizeof(info));
    info.dwMask = PIM_ENCODINGTYPE | PIM_FLAGS | PIM_PROPID;
    hr = IMimeBody_GetPropInfo(body, "Subject", NULL);
    ok(hr == E_INVALIDARG, "ret %08x\n", hr);

    memset(&info, 0xfe, sizeof(info));
    info.dwMask = PIM_ENCODINGTYPE | PIM_FLAGS | PIM_PROPID;
    hr = IMimeBody_GetPropInfo(body, "Subject", &info);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
       ok(info.dwMask & (PIM_ENCODINGTYPE | PIM_FLAGS| PIM_PROPID), "Invalid mask 0x%08x\n", info.dwFlags);
       todo_wine ok(info.dwFlags & 0x10000000, "Invalid flags 0x%08x\n", info.dwFlags);
       ok(info.ietEncoding == 0, "Invalid encoding %d\n", info.ietEncoding);
       ok(info.dwPropId == PID_HDR_SUBJECT, "Invalid propid %d\n", info.dwPropId);
       ok(info.cValues == 0xfefefefe, "Invalid cValues %d\n", info.cValues);
    }

    memset(&info, 0xfe, sizeof(info));
    info.dwMask = 0;
    hr = IMimeBody_GetPropInfo(body, "Subject", &info);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
       ok(info.dwMask == 0, "Invalid mask 0x%08x\n", info.dwFlags);
       ok(info.dwFlags == 0xfefefefe, "Invalid flags 0x%08x\n", info.dwFlags);
       ok(info.ietEncoding == -16843010, "Invalid encoding %d\n", info.ietEncoding);
       ok(info.dwPropId == -16843010, "Invalid propid %d\n", info.dwPropId);
    }

    memset(&info, 0xfe, sizeof(info));
    info.dwMask = 0;
    info.dwPropId = 1024;
    info.ietEncoding = 99;
    hr = IMimeBody_GetPropInfo(body, "Subject", &info);
    ok(hr == S_OK, "ret %08x\n", hr);
    if(hr == S_OK)
    {
       ok(info.dwMask == 0, "Invalid mask 0x%08x\n", info.dwFlags);
       ok(info.dwFlags == 0xfefefefe, "Invalid flags 0x%08x\n", info.dwFlags);
       ok(info.ietEncoding == 99, "Invalid encoding %d\n", info.ietEncoding);
       ok(info.dwPropId == 1024, "Invalid propid %d\n", info.dwPropId);
    }

    memset(&info, 0, sizeof(info));
    info.dwMask = PIM_ENCODINGTYPE | PIM_FLAGS | PIM_PROPID;
    hr = IMimeBody_GetPropInfo(body, "Invalid Property", &info);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    IMimeBody_Release(body);
    IMimeMessage_Release(msg);
}

static void test_MessageOptions(void)
{
    static const char string[] = "XXXXX";
    static const char zero[] =   "0";
    HRESULT hr;
    IMimeMessage *msg;
    PROPVARIANT prop;

    hr = MimeOleCreateMessage(NULL, &msg);
    ok(hr == S_OK, "ret %08x\n", hr);

    PropVariantInit(&prop);

    prop.vt = VT_BOOL;
    prop.u.boolVal = TRUE;
    hr = IMimeMessage_SetOption(msg, OID_HIDE_TNEF_ATTACHMENTS, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    hr = IMimeMessage_GetOption(msg, OID_HIDE_TNEF_ATTACHMENTS, &prop);
    todo_wine ok(hr == S_OK, "ret %08x\n", hr);
    todo_wine ok(prop.vt == VT_BOOL, "vt %08x\n", prop.vt);
    todo_wine ok(prop.u.boolVal == TRUE, "Hide Attachments got %d\n", prop.u.boolVal);
    PropVariantClear(&prop);

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(string)+1);
    strcpy(prop.u.pszVal, string);
    hr = IMimeMessage_SetOption(msg, OID_HIDE_TNEF_ATTACHMENTS, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    hr = IMimeMessage_GetOption(msg, OID_HIDE_TNEF_ATTACHMENTS, &prop);
    todo_wine ok(hr == S_OK, "ret %08x\n", hr);
    todo_wine ok(prop.vt == VT_BOOL, "vt %08x\n", prop.vt);
    todo_wine ok(prop.u.boolVal == TRUE, "Hide Attachments got %d\n", prop.u.boolVal);
    PropVariantClear(&prop);

    /* Invalid property type doesn't change the value */
    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(zero)+1);
    strcpy(prop.u.pszVal, zero);
    hr = IMimeMessage_SetOption(msg, OID_HIDE_TNEF_ATTACHMENTS, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    hr = IMimeMessage_GetOption(msg, OID_HIDE_TNEF_ATTACHMENTS, &prop);
    todo_wine ok(hr == S_OK, "ret %08x\n", hr);
    todo_wine ok(prop.vt == VT_BOOL, "vt %08x\n", prop.vt);
    todo_wine ok(prop.u.boolVal == TRUE, "Hide Attachments got %d\n", prop.u.boolVal);
    PropVariantClear(&prop);

    /* Invalid OID */
    prop.vt = VT_BOOL;
    prop.u.boolVal = TRUE;
    hr = IMimeMessage_SetOption(msg, 0xff00000a, &prop);
    ok(hr == MIME_E_INVALID_OPTION_ID, "ret %08x\n", hr);
    PropVariantClear(&prop);

    /* Out of range before type. */
    prop.vt = VT_I4;
    prop.u.lVal = 1;
    hr = IMimeMessage_SetOption(msg, 0xff00000a, &prop);
    ok(hr == MIME_E_INVALID_OPTION_ID, "ret %08x\n", hr);
    PropVariantClear(&prop);

    IMimeMessage_Release(msg);
}

static void test_BindToObject(void)
{
    HRESULT hr;
    IMimeMessage *msg;
    IMimeBody *body;
    ULONG count;

    hr = MimeOleCreateMessage(NULL, &msg);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeMessage_CountBodies(msg, HBODY_ROOT, TRUE, &count);
    ok(hr == S_OK, "ret %08x\n", hr);
    ok(count == 1, "got %d\n", count);

    hr = IMimeMessage_BindToObject(msg, HBODY_ROOT, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);
    IMimeBody_Release(body);

    IMimeMessage_Release(msg);
}

static void test_BodyDeleteProp(void)
{
    static const char topic[] = "wine topic";
    HRESULT hr;
    IMimeMessage *msg;
    IMimeBody *body;
    PROPVARIANT prop;

    hr = MimeOleCreateMessage(NULL, &msg);
    ok(hr == S_OK, "ret %08x\n", hr);

    PropVariantInit(&prop);

    hr = IMimeMessage_BindToObject(msg, HBODY_ROOT, &IID_IMimeBody, (void**)&body);
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_DeleteProp(body, "Subject");
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    hr = IMimeBody_DeleteProp(body, PIDTOSTR(PID_HDR_SUBJECT));
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, "Subject", 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    hr = IMimeBody_DeleteProp(body, "Subject");
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetProp(body, "Subject", 0, &prop);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    prop.vt = VT_LPSTR;
    prop.u.pszVal = CoTaskMemAlloc(strlen(topic)+1);
    strcpy(prop.u.pszVal, topic);
    hr = IMimeBody_SetProp(body, PIDTOSTR(PID_HDR_SUBJECT), 0, &prop);
    ok(hr == S_OK, "ret %08x\n", hr);
    PropVariantClear(&prop);

    hr = IMimeBody_DeleteProp(body, PIDTOSTR(PID_HDR_SUBJECT));
    ok(hr == S_OK, "ret %08x\n", hr);

    hr = IMimeBody_GetProp(body, PIDTOSTR(PID_HDR_SUBJECT), 0, &prop);
    ok(hr == MIME_E_NOT_FOUND, "ret %08x\n", hr);

    IMimeBody_Release(body);
    IMimeMessage_Release(msg);
}

static void test_MimeOleGetPropertySchema(void)
{
    HRESULT hr;
    IMimePropertySchema *schema = NULL;

    hr = MimeOleGetPropertySchema(&schema);
    ok(hr == S_OK, "ret %08x\n", hr);

    IMimePropertySchema_Release(schema);
}

static void test_mhtml_protocol_info(void)
{
    IInternetProtocolInfo *protocol_info;
    HRESULT hres;

    hres = CoCreateInstance(&CLSID_IMimeHtmlProtocol, NULL, CLSCTX_INPROC_SERVER,
                            &IID_IInternetProtocolInfo, (void**)&protocol_info);
    ok(hres == S_OK, "Could not create protocol info: %08x\n", hres);

    IInternetProtocolInfo_Release(protocol_info);
}

static HRESULT WINAPI outer_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    ok(0, "unexpected call\n");
    return E_NOINTERFACE;
}

static ULONG WINAPI outer_AddRef(IUnknown *iface)
{
    return 2;
}

static ULONG WINAPI outer_Release(IUnknown *iface)
{
    return 1;
}

static const IUnknownVtbl outer_vtbl = {
    outer_QueryInterface,
    outer_AddRef,
    outer_Release
};

static void test_mhtml_protocol(void)
{
    IUnknown outer = { &outer_vtbl };
    IClassFactory *class_factory;
    IUnknown *unk, *unk2;
    HRESULT hres;

    /* test class factory */
    hres = CoGetClassObject(&CLSID_IMimeHtmlProtocol, CLSCTX_INPROC_SERVER, NULL, &IID_IUnknown, (void**)&unk);
    ok(hres == S_OK, "CoGetClassObject failed: %08x\n", hres);

    hres = IUnknown_QueryInterface(unk, &IID_IInternetProtocolInfo, (void**)&unk2);
    ok(hres == E_NOINTERFACE, "IInternetProtocolInfo supported\n");

    hres = IUnknown_QueryInterface(unk, &IID_IClassFactory, (void**)&class_factory);
    ok(hres == S_OK, "Could not get IClassFactory iface: %08x\n", hres);
    IUnknown_Release(unk);

    hres = IClassFactory_CreateInstance(class_factory, &outer, &IID_IUnknown, (void**)&unk);
    ok(hres == S_OK, "CreateInstance returned: %08x\n", hres);
    hres = IUnknown_QueryInterface(unk, &IID_IInternetProtocol, (void**)&unk2);
    ok(hres == S_OK, "Coult not get IInternetProtocol iface: %08x\n", hres);
    IUnknown_Release(unk2);
    IUnknown_Release(unk);

    hres = IClassFactory_CreateInstance(class_factory, (IUnknown*)0xdeadbeef, &IID_IInternetProtocol, (void**)&unk2);
    ok(hres == CLASS_E_NOAGGREGATION, "CreateInstance returned: %08x\n", hres);

    IClassFactory_Release(class_factory);

    test_mhtml_protocol_info();
}

START_TEST(mimeole)
{
    OleInitialize(NULL);
    test_CreateVirtualStream();
    test_CreateSecurity();
    test_CreateBody();
    test_SetData();
    test_Allocator();
    test_CreateMessage();
    test_MessageSetProp();
    test_MessageGetPropInfo();
    test_MessageOptions();
    test_BindToObject();
    test_BodyDeleteProp();
    test_MimeOleGetPropertySchema();
    test_mhtml_protocol();
    OleUninitialize();
}
