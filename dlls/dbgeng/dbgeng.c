/*
 * Support for Microsoft Debugging Extension API
 *
 * Copyright (C) 2010 Volodymyr Shcherbyna
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

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "psapi.h"

#include "initguid.h"
#include "dbgeng.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(dbgeng);

extern NTSTATUS WINAPI NtSuspendProcess(HANDLE handle);
extern NTSTATUS WINAPI NtResumeProcess(HANDLE handle);

struct module_info
{
    DEBUG_MODULE_PARAMETERS params;
};

struct target_process
{
    struct list entry;
    unsigned int pid;
    unsigned int attach_flags;
    HANDLE handle;
    struct
    {
        struct module_info *info;
        unsigned int loaded;
        unsigned int unloaded;
        BOOL initialized;
    } modules;
};

struct debug_client
{
    IDebugClient IDebugClient_iface;
    IDebugDataSpaces IDebugDataSpaces_iface;
    IDebugSymbols3 IDebugSymbols3_iface;
    IDebugControl2 IDebugControl2_iface;
    LONG refcount;
    ULONG engine_options;
    struct list targets;
    IDebugEventCallbacks *event_callbacks;
};

static struct target_process *debug_client_get_target(struct debug_client *debug_client)
{
    if (list_empty(&debug_client->targets))
        return NULL;

    return LIST_ENTRY(list_head(&debug_client->targets), struct target_process, entry);
}

static HRESULT debug_target_init_modules_info(struct target_process *target)
{
    unsigned int i, count;
    HMODULE *modules;
    MODULEINFO info;
    DWORD needed;

    if (target->modules.initialized)
        return S_OK;

    if (!target->handle)
        return E_UNEXPECTED;

    needed = 0;
    EnumProcessModules(target->handle, NULL, 0, &needed);
    if (!needed)
        return E_FAIL;

    count = needed / sizeof(HMODULE);

    if (!(modules = heap_alloc(count * sizeof(*modules))))
        return E_OUTOFMEMORY;

    if (!(target->modules.info = heap_alloc_zero(count * sizeof(*target->modules.info))))
    {
        heap_free(modules);
        return E_OUTOFMEMORY;
    }

    if (EnumProcessModules(target->handle, modules, count * sizeof(*modules), &needed))
    {
        for (i = 0; i < count; ++i)
        {
            if (!GetModuleInformation(target->handle, modules[i], &info, sizeof(info)))
            {
                WARN("Failed to get module information, error %d.\n", GetLastError());
                continue;
            }

            target->modules.info[i].params.Base = (ULONG_PTR)info.lpBaseOfDll;
            target->modules.info[i].params.Size = info.SizeOfImage;
        }
    }

    heap_free(modules);

    target->modules.loaded = count;
    target->modules.unloaded = 0; /* FIXME */

    target->modules.initialized = TRUE;

    return S_OK;
}

static const struct module_info *debug_target_get_module_info(struct target_process *target, unsigned int i)
{
    if (FAILED(debug_target_init_modules_info(target)))
        return NULL;

    if (i >= target->modules.loaded)
        return NULL;

    return &target->modules.info[i];
}

static void debug_client_detach_target(struct target_process *target)
{
    NTSTATUS status;

    if (!target->handle)
        return;

    if (target->attach_flags & DEBUG_ATTACH_NONINVASIVE)
    {
        BOOL resume = !(target->attach_flags & DEBUG_ATTACH_NONINVASIVE_NO_SUSPEND);

        if (resume)
        {
            if ((status = NtResumeProcess(target->handle)))
                WARN("Failed to resume process, status %#x.\n", status);
        }
    }

    CloseHandle(target->handle);
    target->handle = NULL;
}

static struct debug_client *impl_from_IDebugClient(IDebugClient *iface)
{
    return CONTAINING_RECORD(iface, struct debug_client, IDebugClient_iface);
}

static struct debug_client *impl_from_IDebugDataSpaces(IDebugDataSpaces *iface)
{
    return CONTAINING_RECORD(iface, struct debug_client, IDebugDataSpaces_iface);
}

static struct debug_client *impl_from_IDebugSymbols3(IDebugSymbols3 *iface)
{
    return CONTAINING_RECORD(iface, struct debug_client, IDebugSymbols3_iface);
}

static struct debug_client *impl_from_IDebugControl2(IDebugControl2 *iface)
{
    return CONTAINING_RECORD(iface, struct debug_client, IDebugControl2_iface);
}

static HRESULT STDMETHODCALLTYPE debugclient_QueryInterface(IDebugClient *iface, REFIID riid, void **obj)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDebugClient) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
    }
    else if (IsEqualIID(riid, &IID_IDebugDataSpaces))
    {
        *obj = &debug_client->IDebugDataSpaces_iface;
    }
    else if (IsEqualIID(riid, &IID_IDebugSymbols)
             || IsEqualIID(riid, &IID_IDebugSymbols2)
             || IsEqualIID(riid, &IID_IDebugSymbols3))
    {
        *obj = &debug_client->IDebugSymbols3_iface;
    }
    else if (IsEqualIID(riid, &IID_IDebugControl2)
            || IsEqualIID(riid, &IID_IDebugControl))
    {
        *obj = &debug_client->IDebugControl2_iface;
    }
    else
    {
        WARN("Unsupported interface %s.\n", debugstr_guid(riid));
        *obj = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*obj);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE debugclient_AddRef(IDebugClient *iface)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);
    ULONG refcount = InterlockedIncrement(&debug_client->refcount);

    TRACE("%p, %d.\n", iface, refcount);

    return refcount;
}

static void debug_target_free(struct target_process *target)
{
    heap_free(target->modules.info);
    heap_free(target);
}

static ULONG STDMETHODCALLTYPE debugclient_Release(IDebugClient *iface)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);
    ULONG refcount = InterlockedDecrement(&debug_client->refcount);
    struct target_process *cur, *cur2;

    TRACE("%p, %d.\n", debug_client, refcount);

    if (!refcount)
    {
        LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &debug_client->targets, struct target_process, entry)
        {
            debug_client_detach_target(cur);
            list_remove(&cur->entry);
            debug_target_free(cur);
        }
        if (debug_client->event_callbacks)
            debug_client->event_callbacks->lpVtbl->Release(debug_client->event_callbacks);
        heap_free(debug_client);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE debugclient_AttachKernel(IDebugClient *iface, ULONG flags, const char *options)
{
    FIXME("%p, %#x, %s stub.\n", iface, flags, debugstr_a(options));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetKernelConnectionOptions(IDebugClient *iface, char *buffer,
        ULONG buffer_size, ULONG *options_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, options_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetKernelConnectionOptions(IDebugClient *iface, const char *options)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(options));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_StartProcessServer(IDebugClient *iface, ULONG flags, const char *options,
        void *reserved)
{
    FIXME("%p, %#x, %s, %p stub.\n", iface, flags, debugstr_a(options), reserved);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_ConnectProcessServer(IDebugClient *iface, const char *remote_options,
        ULONG64 *server)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_a(remote_options), server);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_DisconnectProcessServer(IDebugClient *iface, ULONG64 server)
{
    FIXME("%p, %s stub.\n", iface, wine_dbgstr_longlong(server));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetRunningProcessSystemIds(IDebugClient *iface, ULONG64 server,
        ULONG *ids, ULONG count, ULONG *actual_count)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(server), ids, count, actual_count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetRunningProcessSystemIdByExecutableName(IDebugClient *iface,
        ULONG64 server, const char *exe_name, ULONG flags, ULONG *id)
{
    FIXME("%p, %s, %s, %#x, %p stub.\n", iface, wine_dbgstr_longlong(server), debugstr_a(exe_name), flags, id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetRunningProcessDescription(IDebugClient *iface, ULONG64 server,
        ULONG systemid, ULONG flags, char *exe_name, ULONG exe_name_size, ULONG *actual_exe_name_size,
        char *description, ULONG description_size, ULONG *actual_description_size)
{
    FIXME("%p, %s, %u, %#x, %p, %u, %p, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(server), systemid, flags,
            exe_name, exe_name_size, actual_exe_name_size, description, description_size, actual_description_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_AttachProcess(IDebugClient *iface, ULONG64 server, ULONG pid, ULONG flags)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);
    struct target_process *process;

    TRACE("%p, %s, %u, %#x.\n", iface, wine_dbgstr_longlong(server), pid, flags);

    if (server)
    {
        FIXME("Remote debugging is not supported.\n");
        return E_NOTIMPL;
    }

    if (!(process = heap_alloc_zero(sizeof(*process))))
        return E_OUTOFMEMORY;

    process->pid = pid;
    process->attach_flags = flags;

    list_add_head(&debug_client->targets, &process->entry);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugclient_CreateProcess(IDebugClient *iface, ULONG64 server, char *cmdline,
        ULONG flags)
{
    FIXME("%p, %s, %s, %#x stub.\n", iface, wine_dbgstr_longlong(server), debugstr_a(cmdline), flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_CreateProcessAndAttach(IDebugClient *iface, ULONG64 server, char *cmdline,
        ULONG create_flags, ULONG pid, ULONG attach_flags)
{
    FIXME("%p, %s, %s, %#x, %u, %#x stub.\n", iface, wine_dbgstr_longlong(server), debugstr_a(cmdline), create_flags,
            pid, attach_flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetProcessOptions(IDebugClient *iface, ULONG *options)
{
    FIXME("%p, %p stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_AddProcessOptions(IDebugClient *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_RemoveProcessOptions(IDebugClient *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetProcessOptions(IDebugClient *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_OpenDumpFile(IDebugClient *iface, const char *filename)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(filename));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_WriteDumpFile(IDebugClient *iface, const char *filename, ULONG qualifier)
{
    FIXME("%p, %s, %u stub.\n", iface, debugstr_a(filename), qualifier);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_ConnectSession(IDebugClient *iface, ULONG flags, ULONG history_limit)
{
    FIXME("%p, %#x, %u stub.\n", iface, flags, history_limit);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_StartServer(IDebugClient *iface, const char *options)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(options));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_OutputServers(IDebugClient *iface, ULONG output_control,
        const char *machine, ULONG flags)
{
    FIXME("%p, %u, %s, %#x stub.\n", iface, output_control, debugstr_a(machine), flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_TerminateProcesses(IDebugClient *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_DetachProcesses(IDebugClient *iface)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);
    struct target_process *target;

    TRACE("%p.\n", iface);

    LIST_FOR_EACH_ENTRY(target, &debug_client->targets, struct target_process, entry)
    {
        debug_client_detach_target(target);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugclient_EndSession(IDebugClient *iface, ULONG flags)
{
    FIXME("%p, %#x stub.\n", iface, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetExitCode(IDebugClient *iface, ULONG *code)
{
    FIXME("%p, %p stub.\n", iface, code);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_DispatchCallbacks(IDebugClient *iface, ULONG timeout)
{
    FIXME("%p, %u stub.\n", iface, timeout);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_ExitDispatch(IDebugClient *iface, IDebugClient *client)
{
    FIXME("%p, %p stub.\n", iface, client);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_CreateClient(IDebugClient *iface, IDebugClient **client)
{
    FIXME("%p, %p stub.\n", iface, client);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetInputCallbacks(IDebugClient *iface, IDebugInputCallbacks **callbacks)
{
    FIXME("%p, %p stub.\n", iface, callbacks);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetInputCallbacks(IDebugClient *iface, IDebugInputCallbacks *callbacks)
{
    FIXME("%p, %p stub.\n", iface, callbacks);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetOutputCallbacks(IDebugClient *iface, IDebugOutputCallbacks **callbacks)
{
    FIXME("%p, %p stub.\n", iface, callbacks);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetOutputCallbacks(IDebugClient *iface, IDebugOutputCallbacks *callbacks)
{
    FIXME("%p, %p stub.\n", iface, callbacks);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetOutputMask(IDebugClient *iface, ULONG *mask)
{
    FIXME("%p, %p stub.\n", iface, mask);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetOutputMask(IDebugClient *iface, ULONG mask)
{
    FIXME("%p, %#x stub.\n", iface, mask);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetOtherOutputMask(IDebugClient *iface, IDebugClient *client, ULONG *mask)
{
    FIXME("%p, %p, %p stub.\n", iface, client, mask);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetOtherOutputMask(IDebugClient *iface, IDebugClient *client, ULONG mask)
{
    FIXME("%p, %p, %#x stub.\n", iface, client, mask);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetOutputWidth(IDebugClient *iface, ULONG *columns)
{
    FIXME("%p, %p stub.\n", iface, columns);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetOutputWidth(IDebugClient *iface, ULONG columns)
{
    FIXME("%p, %u stub.\n", iface, columns);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetOutputLinePrefix(IDebugClient *iface, char *buffer, ULONG buffer_size,
        ULONG *prefix_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, prefix_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetOutputLinePrefix(IDebugClient *iface, const char *prefix)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(prefix));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetIdentity(IDebugClient *iface, char *buffer, ULONG buffer_size,
        ULONG *identity_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, identity_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_OutputIdentity(IDebugClient *iface, ULONG output_control, ULONG flags,
        const char *format)
{
    FIXME("%p, %u, %#x, %s stub.\n", iface, output_control, flags, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugclient_GetEventCallbacks(IDebugClient *iface, IDebugEventCallbacks **callbacks)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);

    TRACE("%p, %p.\n", iface, callbacks);

    if (debug_client->event_callbacks)
    {
        *callbacks = debug_client->event_callbacks;
        (*callbacks)->lpVtbl->AddRef(*callbacks);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugclient_SetEventCallbacks(IDebugClient *iface, IDebugEventCallbacks *callbacks)
{
    struct debug_client *debug_client = impl_from_IDebugClient(iface);

    TRACE("%p, %p.\n", iface, callbacks);

    if (debug_client->event_callbacks)
        debug_client->event_callbacks->lpVtbl->Release(debug_client->event_callbacks);
    if ((debug_client->event_callbacks = callbacks))
        debug_client->event_callbacks->lpVtbl->AddRef(debug_client->event_callbacks);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugclient_FlushCallbacks(IDebugClient *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static const IDebugClientVtbl debugclientvtbl =
{
    debugclient_QueryInterface,
    debugclient_AddRef,
    debugclient_Release,
    debugclient_AttachKernel,
    debugclient_GetKernelConnectionOptions,
    debugclient_SetKernelConnectionOptions,
    debugclient_StartProcessServer,
    debugclient_ConnectProcessServer,
    debugclient_DisconnectProcessServer,
    debugclient_GetRunningProcessSystemIds,
    debugclient_GetRunningProcessSystemIdByExecutableName,
    debugclient_GetRunningProcessDescription,
    debugclient_AttachProcess,
    debugclient_CreateProcess,
    debugclient_CreateProcessAndAttach,
    debugclient_GetProcessOptions,
    debugclient_AddProcessOptions,
    debugclient_RemoveProcessOptions,
    debugclient_SetProcessOptions,
    debugclient_OpenDumpFile,
    debugclient_WriteDumpFile,
    debugclient_ConnectSession,
    debugclient_StartServer,
    debugclient_OutputServers,
    debugclient_TerminateProcesses,
    debugclient_DetachProcesses,
    debugclient_EndSession,
    debugclient_GetExitCode,
    debugclient_DispatchCallbacks,
    debugclient_ExitDispatch,
    debugclient_CreateClient,
    debugclient_GetInputCallbacks,
    debugclient_SetInputCallbacks,
    debugclient_GetOutputCallbacks,
    debugclient_SetOutputCallbacks,
    debugclient_GetOutputMask,
    debugclient_SetOutputMask,
    debugclient_GetOtherOutputMask,
    debugclient_SetOtherOutputMask,
    debugclient_GetOutputWidth,
    debugclient_SetOutputWidth,
    debugclient_GetOutputLinePrefix,
    debugclient_SetOutputLinePrefix,
    debugclient_GetIdentity,
    debugclient_OutputIdentity,
    debugclient_GetEventCallbacks,
    debugclient_SetEventCallbacks,
    debugclient_FlushCallbacks,
};

static HRESULT STDMETHODCALLTYPE debugdataspaces_QueryInterface(IDebugDataSpaces *iface, REFIID riid, void **obj)
{
    struct debug_client *debug_client = impl_from_IDebugDataSpaces(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_QueryInterface(unk, riid, obj);
}

static ULONG STDMETHODCALLTYPE debugdataspaces_AddRef(IDebugDataSpaces *iface)
{
    struct debug_client *debug_client = impl_from_IDebugDataSpaces(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_AddRef(unk);
}

static ULONG STDMETHODCALLTYPE debugdataspaces_Release(IDebugDataSpaces *iface)
{
    struct debug_client *debug_client = impl_from_IDebugDataSpaces(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_Release(unk);
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadVirtual(IDebugDataSpaces *iface, ULONG64 offset, void *buffer,
        ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WriteVirtual(IDebugDataSpaces *iface, ULONG64 offset, void *buffer,
        ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_SearchVirtual(IDebugDataSpaces *iface, ULONG64 offset, ULONG64 length,
        void *pattern, ULONG pattern_size, ULONG pattern_granularity, ULONG64 *ret_offset)
{
    FIXME("%p, %s, %s, %p, %u, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), wine_dbgstr_longlong(length),
            pattern, pattern_size, pattern_granularity, ret_offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadVirtualUncached(IDebugDataSpaces *iface, ULONG64 offset,
        void *buffer, ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WriteVirtualUncached(IDebugDataSpaces *iface, ULONG64 offset,
        void *buffer, ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadPointersVirtual(IDebugDataSpaces *iface, ULONG count,
        ULONG64 offset, ULONG64 *pointers)
{
    FIXME("%p, %u, %s, %p stub.\n", iface, count, wine_dbgstr_longlong(offset), pointers);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WritePointersVirtual(IDebugDataSpaces *iface, ULONG count,
        ULONG64 offset, ULONG64 *pointers)
{
    FIXME("%p, %u, %s, %p stub.\n", iface, count, wine_dbgstr_longlong(offset), pointers);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadPhysical(IDebugDataSpaces *iface, ULONG64 offset, void *buffer,
        ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WritePhysical(IDebugDataSpaces *iface, ULONG64 offset, void *buffer,
        ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadControl(IDebugDataSpaces *iface, ULONG processor, ULONG64 offset,
        void *buffer, ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %u, %s, %p, %u, %p stub.\n", iface, processor, wine_dbgstr_longlong(offset), buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WriteControl(IDebugDataSpaces *iface, ULONG processor, ULONG64 offset,
        void *buffer, ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %u, %s, %p, %u, %p stub.\n", iface, processor, wine_dbgstr_longlong(offset), buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadIo(IDebugDataSpaces *iface, ULONG type, ULONG bus_number,
        ULONG address_space, ULONG64 offset, void *buffer, ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %u, %u, %u, %s, %p, %u, %p stub.\n", iface, type, bus_number, address_space, wine_dbgstr_longlong(offset),
            buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WriteIo(IDebugDataSpaces *iface, ULONG type, ULONG bus_number,
        ULONG address_space, ULONG64 offset, void *buffer, ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %u, %u, %u, %s, %p, %u, %p stub.\n", iface, type, bus_number, address_space, wine_dbgstr_longlong(offset),
            buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadMsr(IDebugDataSpaces *iface, ULONG msr, ULONG64 *value)
{
    FIXME("%p, %u, %p stub.\n", iface, msr, value);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WriteMsr(IDebugDataSpaces *iface, ULONG msr, ULONG64 value)
{
    FIXME("%p, %u, %s stub.\n", iface, msr, wine_dbgstr_longlong(value));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadBusData(IDebugDataSpaces *iface, ULONG data_type,
        ULONG bus_number, ULONG slot_number, ULONG offset, void *buffer, ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %u, %u, %u, %u, %p, %u, %p stub.\n", iface, data_type, bus_number, slot_number, offset, buffer,
            buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_WriteBusData(IDebugDataSpaces *iface, ULONG data_type,
        ULONG bus_number, ULONG slot_number, ULONG offset, void *buffer, ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %u, %u, %u, %u, %p, %u, %p stub.\n", iface, data_type, bus_number, slot_number, offset, buffer,
            buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_CheckLowMemory(IDebugDataSpaces *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadDebuggerData(IDebugDataSpaces *iface, ULONG index, void *buffer,
        ULONG buffer_size, ULONG *data_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, data_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugdataspaces_ReadProcessorSystemData(IDebugDataSpaces *iface, ULONG processor,
        ULONG index, void *buffer, ULONG buffer_size, ULONG *data_size)
{
    FIXME("%p, %u, %u, %p, %u, %p stub.\n", iface, processor, index, buffer, buffer_size, data_size);

    return E_NOTIMPL;
}

static const IDebugDataSpacesVtbl debugdataspacesvtbl =
{
    debugdataspaces_QueryInterface,
    debugdataspaces_AddRef,
    debugdataspaces_Release,
    debugdataspaces_ReadVirtual,
    debugdataspaces_WriteVirtual,
    debugdataspaces_SearchVirtual,
    debugdataspaces_ReadVirtualUncached,
    debugdataspaces_WriteVirtualUncached,
    debugdataspaces_ReadPointersVirtual,
    debugdataspaces_WritePointersVirtual,
    debugdataspaces_ReadPhysical,
    debugdataspaces_WritePhysical,
    debugdataspaces_ReadControl,
    debugdataspaces_WriteControl,
    debugdataspaces_ReadIo,
    debugdataspaces_WriteIo,
    debugdataspaces_ReadMsr,
    debugdataspaces_WriteMsr,
    debugdataspaces_ReadBusData,
    debugdataspaces_WriteBusData,
    debugdataspaces_CheckLowMemory,
    debugdataspaces_ReadDebuggerData,
    debugdataspaces_ReadProcessorSystemData,
};

static HRESULT STDMETHODCALLTYPE debugsymbols_QueryInterface(IDebugSymbols3 *iface, REFIID riid, void **obj)
{
    struct debug_client *debug_client = impl_from_IDebugSymbols3(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_QueryInterface(unk, riid, obj);
}

static ULONG STDMETHODCALLTYPE debugsymbols_AddRef(IDebugSymbols3 *iface)
{
    struct debug_client *debug_client = impl_from_IDebugSymbols3(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_AddRef(unk);
}

static ULONG STDMETHODCALLTYPE debugsymbols_Release(IDebugSymbols3 *iface)
{
    struct debug_client *debug_client = impl_from_IDebugSymbols3(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_Release(unk);
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolOptions(IDebugSymbols3 *iface, ULONG *options)
{
    FIXME("%p, %p stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AddSymbolOptions(IDebugSymbols3 *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_RemoveSymbolOptions(IDebugSymbols3 *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetSymbolOptions(IDebugSymbols3 *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNameByOffset(IDebugSymbols3 *iface, ULONG64 offset, char *buffer,
        ULONG buffer_size, ULONG *name_size, ULONG64 *displacement)
{
    FIXME("%p, %s, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size,
            name_size, displacement);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetOffsetByName(IDebugSymbols3 *iface, const char *symbol,
        ULONG64 *offset)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_a(symbol), offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNearNameByOffset(IDebugSymbols3 *iface, ULONG64 offset, LONG delta,
        char *buffer, ULONG buffer_size, ULONG *name_size, ULONG64 *displacement)
{
    FIXME("%p, %s, %d, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), delta, buffer, buffer_size,
            name_size, displacement);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetLineByOffset(IDebugSymbols3 *iface, ULONG64 offset, ULONG *line,
        char *buffer, ULONG buffer_size, ULONG *file_size, ULONG64 *displacement)
{
    FIXME("%p, %s, %p, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), line, buffer, buffer_size,
            file_size, displacement);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetOffsetByLine(IDebugSymbols3 *iface, ULONG line, const char *file,
        ULONG64 *offset)
{
    FIXME("%p, %u, %s, %p stub.\n", iface, line, debugstr_a(file), offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNumberModules(IDebugSymbols3 *iface, ULONG *loaded, ULONG *unloaded)
{
    struct debug_client *debug_client = impl_from_IDebugSymbols3(iface);
    static struct target_process *target;
    HRESULT hr;

    TRACE("%p, %p, %p.\n", iface, loaded, unloaded);

    if (!(target = debug_client_get_target(debug_client)))
        return E_UNEXPECTED;

    if (FAILED(hr = debug_target_init_modules_info(target)))
        return hr;

    *loaded = target->modules.loaded;
    *unloaded = target->modules.unloaded;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByIndex(IDebugSymbols3 *iface, ULONG index, ULONG64 *base)
{
    struct debug_client *debug_client = impl_from_IDebugSymbols3(iface);
    const struct module_info *info;
    struct target_process *target;

    TRACE("%p, %u, %p.\n", iface, index, base);

    if (!(target = debug_client_get_target(debug_client)))
        return E_UNEXPECTED;

    if (!(info = debug_target_get_module_info(target, index)))
        return E_INVALIDARG;

    *base = info->params.Base;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByModuleName(IDebugSymbols3 *iface, const char *name,
        ULONG start_index, ULONG *index, ULONG64 *base)
{
    FIXME("%p, %s, %u, %p, %p stub.\n", iface, debugstr_a(name), start_index, index, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByOffset(IDebugSymbols3 *iface, ULONG64 offset,
        ULONG start_index, ULONG *index, ULONG64 *base)
{
    FIXME("%p, %s, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), start_index, index, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleNames(IDebugSymbols3 *iface, ULONG index, ULONG64 base,
        char *image_name, ULONG image_name_buffer_size, ULONG *image_name_size, char *module_name,
        ULONG module_name_buffer_size, ULONG *module_name_size, char *loaded_image_name,
        ULONG loaded_image_name_buffer_size, ULONG *loaded_image_size)
{
    FIXME("%p, %u, %s, %p, %u, %p, %p, %u, %p, %p, %u, %p stub.\n", iface, index, wine_dbgstr_longlong(base),
            image_name, image_name_buffer_size, image_name_size, module_name, module_name_buffer_size,
            module_name_size, loaded_image_name, loaded_image_name_buffer_size, loaded_image_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleParameters(IDebugSymbols3 *iface, ULONG count, ULONG64 *bases,
        ULONG start, DEBUG_MODULE_PARAMETERS *parameters)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, count, bases, start, parameters);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolModule(IDebugSymbols3 *iface, const char *symbol, ULONG64 *base)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_a(symbol), base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetTypeName(IDebugSymbols3 *iface, ULONG64 base, ULONG type_id,
        char *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %s, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(base), type_id, buffer,
            buffer_size, name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetTypeId(IDebugSymbols3 *iface, ULONG64 base, const char *name,
        ULONG *type_id)
{
    FIXME("%p, %s, %s, %p stub.\n", iface, wine_dbgstr_longlong(base), debugstr_a(name), type_id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetTypeSize(IDebugSymbols3 *iface, ULONG64 base, ULONG type_id,
        ULONG *size)
{
    FIXME("%p, %s, %u, %p stub.\n", iface, wine_dbgstr_longlong(base), type_id, size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFieldOffset(IDebugSymbols3 *iface, ULONG64 base, ULONG type_id,
        const char *field, ULONG *offset)
{
    FIXME("%p, %s, %u, %s, %p stub.\n", iface, wine_dbgstr_longlong(base), type_id, debugstr_a(field), offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolTypeId(IDebugSymbols3 *iface, const char *symbol, ULONG *type_id,
        ULONG64 *base)
{
    FIXME("%p, %s, %p, %p stub.\n", iface, debugstr_a(symbol), type_id, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetOffsetTypeId(IDebugSymbols3 *iface, ULONG64 offset, ULONG *type_id,
        ULONG64 *base)
{
    FIXME("%p, %s, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), type_id, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_ReadTypedDataVirtual(IDebugSymbols3 *iface, ULONG64 offset, ULONG64 base,
        ULONG type_id, void *buffer, ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %s, %s, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), wine_dbgstr_longlong(base),
            type_id, buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_WriteTypedDataVirtual(IDebugSymbols3 *iface, ULONG64 offset, ULONG64 base,
        ULONG type_id, void *buffer, ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %s, %s, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), wine_dbgstr_longlong(base),
            type_id, buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_OutputTypedDataVirtual(IDebugSymbols3 *iface, ULONG output_control,
        ULONG64 offset, ULONG64 base, ULONG type_id, ULONG flags)
{
    FIXME("%p, %#x, %s, %s, %u, %#x stub.\n", iface, output_control, wine_dbgstr_longlong(offset),
            wine_dbgstr_longlong(base), type_id, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_ReadTypedDataPhysical(IDebugSymbols3 *iface, ULONG64 offset, ULONG64 base,
        ULONG type_id, void *buffer, ULONG buffer_size, ULONG *read_len)
{
    FIXME("%p, %s, %s, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), wine_dbgstr_longlong(base),
            type_id, buffer, buffer_size, read_len);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_WriteTypedDataPhysical(IDebugSymbols3 *iface, ULONG64 offset,
        ULONG64 base, ULONG type_id, void *buffer, ULONG buffer_size, ULONG *written)
{
    FIXME("%p, %s, %s, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), wine_dbgstr_longlong(base),
            type_id, buffer, buffer_size, written);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_OutputTypedDataPhysical(IDebugSymbols3 *iface, ULONG output_control,
        ULONG64 offset, ULONG64 base, ULONG type_id, ULONG flags)
{
    FIXME("%p, %#x, %s, %s, %u, %#x stub.\n", iface, output_control, wine_dbgstr_longlong(offset),
            wine_dbgstr_longlong(base), type_id, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetScope(IDebugSymbols3 *iface, ULONG64 *instr_offset,
        DEBUG_STACK_FRAME *frame, void *scope_context, ULONG scope_context_size)
{
    FIXME("%p, %p, %p, %p, %u stub.\n", iface, instr_offset, frame, scope_context, scope_context_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetScope(IDebugSymbols3 *iface, ULONG64 instr_offset,
        DEBUG_STACK_FRAME *frame, void *scope_context, ULONG scope_context_size)
{
    FIXME("%p, %s, %p, %p, %u stub.\n", iface, wine_dbgstr_longlong(instr_offset), frame, scope_context,
            scope_context_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_ResetScope(IDebugSymbols3 *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetScopeSymbolGroup(IDebugSymbols3 *iface, ULONG flags,
        IDebugSymbolGroup *update, IDebugSymbolGroup **symbols)
{
    FIXME("%p, %#x, %p, %p stub.\n", iface, flags, update, symbols);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_CreateSymbolGroup(IDebugSymbols3 *iface, IDebugSymbolGroup **group)
{
    FIXME("%p, %p stub.\n", iface, group);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_StartSymbolMatch(IDebugSymbols3 *iface, const char *pattern,
        ULONG64 *handle)
{
    FIXME("%p, %s, %p stub.\n", iface, pattern, handle);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNextSymbolMatch(IDebugSymbols3 *iface, ULONG64 handle, char *buffer,
        ULONG buffer_size, ULONG *match_size, ULONG64 *offset)
{
    FIXME("%p, %s, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(handle), buffer, buffer_size, match_size, offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_EndSymbolMatch(IDebugSymbols3 *iface, ULONG64 handle)
{
    FIXME("%p, %s stub.\n", iface, wine_dbgstr_longlong(handle));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_Reload(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolPath(IDebugSymbols3 *iface, char *buffer, ULONG buffer_size,
        ULONG *path_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, path_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetSymbolPath(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AppendSymbolPath(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetImagePath(IDebugSymbols3 *iface, char *buffer, ULONG buffer_size,
        ULONG *path_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, path_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetImagePath(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AppendImagePath(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourcePath(IDebugSymbols3 *iface, char *buffer, ULONG buffer_size,
        ULONG *path_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, path_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourcePathElement(IDebugSymbols3 *iface, ULONG index, char *buffer,
        ULONG buffer_size, ULONG *element_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, element_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetSourcePath(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AppendSourcePath(IDebugSymbols3 *iface, const char *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_FindSourceFile(IDebugSymbols3 *iface, ULONG start, const char *file,
        ULONG flags, ULONG *found_element, char *buffer, ULONG buffer_size, ULONG *found_size)
{
    FIXME("%p, %u, %s, %#x, %p, %p, %u, %p stub.\n", iface, start, debugstr_a(file), flags, found_element, buffer,
            buffer_size, found_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceFileLineOffsets(IDebugSymbols3 *iface, const char *file,
        ULONG64 *buffer, ULONG buffer_lines, ULONG *file_lines)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, debugstr_a(file), buffer, buffer_lines, file_lines);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleVersionInformation(IDebugSymbols3 *iface, ULONG index,
        ULONG64 base, const char *item, void *buffer, ULONG buffer_size, ULONG *info_size)
{
    FIXME("%p, %u, %s, %s, %p, %u, %p stub.\n", iface, index, wine_dbgstr_longlong(base), debugstr_a(item), buffer,
            buffer_size, info_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleNameString(IDebugSymbols3 *iface, ULONG which, ULONG index,
        ULONG64 base, char *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %u, %u, %s, %p, %u, %p stub.\n", iface, which, index, wine_dbgstr_longlong(base), buffer, buffer_size,
            name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetConstantName(IDebugSymbols3 *iface, ULONG64 module, ULONG type_id,
        ULONG64 value, char *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %s, %u, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(module), type_id,
            wine_dbgstr_longlong(value), buffer, buffer_size, name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFieldName(IDebugSymbols3 *iface, ULONG64 module, ULONG type_id,
        ULONG field_index, char *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %s, %u, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(module), type_id, field_index, buffer,
            buffer_size, name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetTypeOptions(IDebugSymbols3 *iface, ULONG *options)
{
    FIXME("%p, %p stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AddTypeOptions(IDebugSymbols3 *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_RemoveTypeOptions(IDebugSymbols3 *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetTypeOptions(IDebugSymbols3 *iface, ULONG options)
{
    FIXME("%p, %#x stub.\n", iface, options);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNameByOffsetWide(IDebugSymbols3 *iface, ULONG64 offset, WCHAR *buffer,
        ULONG buffer_size, ULONG *name_size, ULONG64 *displacement)
{
    FIXME("%p, %s, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), buffer, buffer_size, name_size,
            displacement);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetOffsetByNameWide(IDebugSymbols3 *iface, const WCHAR *symbol,
        ULONG64 *offset)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_w(symbol), offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNearNameByOffsetWide(IDebugSymbols3 *iface, ULONG64 offset,
        LONG delta, WCHAR *buffer, ULONG buffer_size, ULONG *name_size, ULONG64 *displacement)
{
    FIXME("%p, %s, %d, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), delta, buffer, buffer_size,
            name_size, displacement);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetLineByOffsetWide(IDebugSymbols3 *iface, ULONG64 offset, ULONG *line,
        WCHAR *buffer, ULONG buffer_size, ULONG *file_size, ULONG64 *displacement)
{
    FIXME("%p, %s, %p, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), line, buffer, buffer_size,
            file_size, displacement);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetOffsetByLineWide(IDebugSymbols3 *iface, ULONG line, const WCHAR *file,
        ULONG64 *offset)
{
    FIXME("%p, %u, %s, %p stub.\n", iface, line, debugstr_w(file), offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByModuleNameWide(IDebugSymbols3 *iface, const WCHAR *name,
        ULONG start_index, ULONG *index, ULONG64 *base)
{
    FIXME("%p, %s, %u, %p, %p stub.\n", iface, debugstr_w(name), start_index, index, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolModuleWide(IDebugSymbols3 *iface, const WCHAR *symbol,
        ULONG64 *base)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_w(symbol), base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetTypeNameWide(IDebugSymbols3 *iface, ULONG64 module, ULONG type_id,
        WCHAR *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %s, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(module), type_id, buffer, buffer_size,
            name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetTypeIdWide(IDebugSymbols3 *iface, ULONG64 module, const WCHAR *name,
        ULONG *type_id)
{
    FIXME("%p, %s, %s, %p stub.\n", iface, wine_dbgstr_longlong(module), debugstr_w(name), type_id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFieldOffsetWide(IDebugSymbols3 *iface, ULONG64 module, ULONG type_id,
        const WCHAR *field, ULONG *offset)
{
    FIXME("%p, %s, %u, %s, %p stub.\n", iface, wine_dbgstr_longlong(module), type_id, debugstr_w(field), offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolTypeIdWide(IDebugSymbols3 *iface, const WCHAR *symbol,
        ULONG *type_id, ULONG64 *module)
{
    FIXME("%p, %s, %p, %p stub.\n", iface, debugstr_w(symbol), type_id, module);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetScopeSymbolGroup2(IDebugSymbols3 *iface, ULONG flags,
        PDEBUG_SYMBOL_GROUP2 update, PDEBUG_SYMBOL_GROUP2 *symbols)
{
    FIXME("%p, %#x, %p, %p stub.\n", iface, flags, update, symbols);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_CreateSymbolGroup2(IDebugSymbols3 *iface, PDEBUG_SYMBOL_GROUP2 *group)
{
    FIXME("%p, %p stub.\n", iface, group);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_StartSymbolMatchWide(IDebugSymbols3 *iface, const WCHAR *pattern,
        ULONG64 *handle)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_w(pattern), handle);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetNextSymbolMatchWide(IDebugSymbols3 *iface, ULONG64 handle,
        WCHAR *buffer, ULONG buffer_size, ULONG *match_size, ULONG64 *offset)
{
    FIXME("%p, %s, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(handle), buffer, buffer_size, match_size, offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_ReloadWide(IDebugSymbols3 *iface, const WCHAR *module)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(module));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolPathWide(IDebugSymbols3 *iface, WCHAR *buffer, ULONG buffer_size,
        ULONG *path_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, path_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetSymbolPathWide(IDebugSymbols3 *iface, const WCHAR *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AppendSymbolPathWide(IDebugSymbols3 *iface, const WCHAR *addition)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(addition));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetImagePathWide(IDebugSymbols3 *iface, WCHAR *buffer, ULONG buffer_size,
        ULONG *path_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, path_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetImagePathWide(IDebugSymbols3 *iface, const WCHAR *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AppendImagePathWide(IDebugSymbols3 *iface, const WCHAR *addition)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(addition));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourcePathWide(IDebugSymbols3 *iface, WCHAR *buffer, ULONG buffer_size,
        ULONG *path_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, path_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourcePathElementWide(IDebugSymbols3 *iface, ULONG index,
        WCHAR *buffer, ULONG buffer_size, ULONG *element_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, element_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetSourcePathWide(IDebugSymbols3 *iface, const WCHAR *path)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(path));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AppendSourcePathWide(IDebugSymbols3 *iface, const WCHAR *addition)
{
    FIXME("%p, %s stub.\n", iface, debugstr_w(addition));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_FindSourceFileWide(IDebugSymbols3 *iface, ULONG start_element,
        const WCHAR *file, ULONG flags, ULONG *found_element, WCHAR *buffer, ULONG buffer_size, ULONG *found_size)
{
    FIXME("%p, %u, %s, %#x, %p, %p, %u, %p stub.\n", iface, start_element, debugstr_w(file), flags, found_element,
            buffer, buffer_size, found_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceFileLineOffsetsWide(IDebugSymbols3 *iface, const WCHAR *file,
        ULONG64 *buffer, ULONG buffer_lines, ULONG *file_lines)
{
    FIXME("%p, %s, %p, %u, %p stub.\n", iface, debugstr_w(file), buffer, buffer_lines, file_lines);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleVersionInformationWide(IDebugSymbols3 *iface, ULONG index,
        ULONG64 base, const WCHAR *item, void *buffer, ULONG buffer_size, ULONG *version_info_size)
{
    FIXME("%p, %u, %s, %s, %p, %u, %p stub.\n", iface, index, wine_dbgstr_longlong(base), debugstr_w(item), buffer,
            buffer_size, version_info_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleNameStringWide(IDebugSymbols3 *iface, ULONG which, ULONG index,
        ULONG64 base, WCHAR *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %u, %u, %s, %p, %u, %p stub.\n", iface, which, index, wine_dbgstr_longlong(base), buffer, buffer_size,
            name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetConstantNameWide(IDebugSymbols3 *iface, ULONG64 module, ULONG type_id,
        ULONG64 value, WCHAR *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %s, %u, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(module), type_id,
            wine_dbgstr_longlong(value), buffer, buffer_size, name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFieldNameWide(IDebugSymbols3 *iface, ULONG64 module, ULONG type_id,
        ULONG field_index, WCHAR *buffer, ULONG buffer_size, ULONG *name_size)
{
    FIXME("%p, %s, %u, %u, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(module), type_id, field_index, buffer,
            buffer_size, name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_IsManagedModule(IDebugSymbols3 *iface, ULONG index, ULONG64 base)
{
    FIXME("%p, %u, %s stub.\n", iface, index, wine_dbgstr_longlong(base));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByModuleName2(IDebugSymbols3 *iface, const char *name,
        ULONG start_index, ULONG flags, ULONG *index, ULONG64 *base)
{
    FIXME("%p, %s, %u, %#x, %p, %p stub.\n", iface, debugstr_a(name), start_index, flags, index, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByModuleName2Wide(IDebugSymbols3 *iface, const WCHAR *name,
        ULONG start_index, ULONG flags, ULONG *index, ULONG64 *base)
{
    FIXME("%p, %s, %u, %#x, %p, %p stub.\n", iface, debugstr_w(name), start_index, flags, index, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetModuleByOffset2(IDebugSymbols3 *iface, ULONG64 offset,
        ULONG start_index, ULONG flags, ULONG *index, ULONG64 *base)
{
    FIXME("%p, %s, %u, %#x, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), start_index, flags, index, base);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AddSyntheticModule(IDebugSymbols3 *iface, ULONG64 base, ULONG size,
        const char *image_path, const char *module_name, ULONG flags)
{
    FIXME("%p, %s, %u, %s, %s, %#x stub.\n", iface, wine_dbgstr_longlong(base), size, debugstr_a(image_path),
            debugstr_a(module_name), flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AddSyntheticModuleWide(IDebugSymbols3 *iface, ULONG64 base, ULONG size,
        const WCHAR *image_path, const WCHAR *module_name, ULONG flags)
{
    FIXME("%p, %s, %u, %s, %s, %#x stub.\n", iface, wine_dbgstr_longlong(base), size, debugstr_w(image_path),
            debugstr_w(module_name), flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_RemoveSyntheticModule(IDebugSymbols3 *iface, ULONG64 base)
{
    FIXME("%p, %s stub.\n", iface, wine_dbgstr_longlong(base));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetCurrentScopeFrameIndex(IDebugSymbols3 *iface, ULONG *index)
{
    FIXME("%p, %p stub.\n", iface, index);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetScopeFrameByIndex(IDebugSymbols3 *iface, ULONG index)
{
    FIXME("%p, %u stub.\n", iface, index);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetScopeFromJitDebugInfo(IDebugSymbols3 *iface, ULONG output_control,
        ULONG64 info_offset)
{
    FIXME("%p, %u, %s stub.\n", iface, output_control, wine_dbgstr_longlong(info_offset));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_SetScopeFromStoredEvent(IDebugSymbols3 *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_OutputSymbolByOffset(IDebugSymbols3 *iface, ULONG output_control,
        ULONG flags, ULONG64 offset)
{
    FIXME("%p, %u, %#x, %s stub.\n", iface, output_control, flags, wine_dbgstr_longlong(offset));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFunctionEntryByOffset(IDebugSymbols3 *iface, ULONG64 offset,
        ULONG flags, void *buffer, ULONG buffer_size, ULONG *needed_size)
{
    FIXME("%p, %s, %#x, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), flags, buffer, buffer_size,
            needed_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFieldTypeAndOffset(IDebugSymbols3 *iface, ULONG64 module,
        ULONG container_type_id, const char *field, ULONG *field_type_id, ULONG *offset)
{
    FIXME("%p, %s, %u, %s, %p, %p stub.\n", iface, wine_dbgstr_longlong(module), container_type_id, debugstr_a(field),
            field_type_id, offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetFieldTypeAndOffsetWide(IDebugSymbols3 *iface, ULONG64 module,
        ULONG container_type_id, const WCHAR *field, ULONG *field_type_id, ULONG *offset)
{
    FIXME("%p, %s, %u, %s, %p, %p stub.\n", iface, wine_dbgstr_longlong(module), container_type_id, debugstr_w(field),
            field_type_id, offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AddSyntheticSymbol(IDebugSymbols3 *iface, ULONG64 offset, ULONG size,
        const char *name, ULONG flags, DEBUG_MODULE_AND_ID *id)
{
    FIXME("%p, %s, %u, %s, %#x, %p stub.\n", iface, wine_dbgstr_longlong(offset), size, debugstr_a(name), flags, id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_AddSyntheticSymbolWide(IDebugSymbols3 *iface, ULONG64 offset, ULONG size,
        const WCHAR *name, ULONG flags, DEBUG_MODULE_AND_ID *id)
{
    FIXME("%p, %s, %u, %s, %#x, %p stub.\n", iface, wine_dbgstr_longlong(offset), size, debugstr_w(name), flags, id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_RemoveSyntheticSymbol(IDebugSymbols3 *iface, DEBUG_MODULE_AND_ID *id)
{
    FIXME("%p, %p stub.\n", iface, id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntriesByOffset(IDebugSymbols3 *iface, ULONG64 offset,
        ULONG flags, DEBUG_MODULE_AND_ID *ids, LONG64 *displacements, ULONG count, ULONG *entries)
{
    FIXME("%p, %s, %#x, %p, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), flags, ids, displacements, count,
            entries);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntriesByName(IDebugSymbols3 *iface, const char *symbol,
        ULONG flags, DEBUG_MODULE_AND_ID *ids, ULONG count, ULONG *entries)
{
    FIXME("%p, %s, %#x, %p, %u, %p stub.\n", iface, debugstr_a(symbol), flags, ids, count, entries);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntriesByNameWide(IDebugSymbols3 *iface, const WCHAR *symbol,
        ULONG flags, DEBUG_MODULE_AND_ID *ids, ULONG count, ULONG *entries)
{
    FIXME("%p, %s, %#x, %p, %u, %p stub.\n", iface, debugstr_w(symbol), flags, ids, count, entries);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntryByToken(IDebugSymbols3 *iface, ULONG64 base, ULONG token,
        DEBUG_MODULE_AND_ID *id)
{
    FIXME("%p, %s, %p stub.\n", iface, wine_dbgstr_longlong(base), id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntryInformation(IDebugSymbols3 *iface, DEBUG_MODULE_AND_ID *id,
        DEBUG_SYMBOL_ENTRY *info)
{
    FIXME("%p, %p, %p stub.\n", iface, id, info);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntryString(IDebugSymbols3 *iface, DEBUG_MODULE_AND_ID *id,
        ULONG which, char *buffer, ULONG buffer_size, ULONG *string_size)
{
    FIXME("%p, %p, %u, %p, %u, %p stub.\n", iface, id, which, buffer, buffer_size, string_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntryStringWide(IDebugSymbols3 *iface, DEBUG_MODULE_AND_ID *id,
        ULONG which, WCHAR *buffer, ULONG buffer_size, ULONG *string_size)
{
    FIXME("%p, %p, %u, %p, %u, %p stub.\n", iface, id, which, buffer, buffer_size, string_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntryOffsetRegions(IDebugSymbols3 *iface, DEBUG_MODULE_AND_ID *id,
        ULONG flags, DEBUG_OFFSET_REGION *regions, ULONG regions_count, ULONG *regions_avail)
{
    FIXME("%p, %p, %#x, %p, %u, %p stub.\n", iface, id, flags, regions, regions_count, regions_avail);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSymbolEntryBySymbolEntry(IDebugSymbols3 *iface,
        DEBUG_MODULE_AND_ID *from_id, ULONG flags, DEBUG_MODULE_AND_ID *to_id)
{
    FIXME("%p, %p, %#x, %p stub.\n", iface, from_id, flags, to_id);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntriesByOffset(IDebugSymbols3 *iface, ULONG64 offset,
        ULONG flags, DEBUG_SYMBOL_SOURCE_ENTRY *entries, ULONG count, ULONG *entries_avail)
{
    FIXME("%p, %s, %#x, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(offset), flags, entries, count, entries_avail);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntriesByLine(IDebugSymbols3 *iface, ULONG line,
        const char *file, ULONG flags, DEBUG_SYMBOL_SOURCE_ENTRY *entries, ULONG count, ULONG *entries_avail)
{
    FIXME("%p, %s, %#x, %p, %u, %p stub.\n", iface, debugstr_a(file), flags, entries, count, entries_avail);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntriesByLineWide(IDebugSymbols3 *iface, ULONG line,
        const WCHAR *file, ULONG flags, DEBUG_SYMBOL_SOURCE_ENTRY *entries, ULONG count, ULONG *entries_avail)
{
    FIXME("%p, %s, %#x, %p, %u, %p stub.\n", iface, debugstr_w(file), flags, entries, count, entries_avail);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntryString(IDebugSymbols3 *iface,
        DEBUG_SYMBOL_SOURCE_ENTRY *entry, ULONG which, char *buffer, ULONG buffer_size, ULONG *string_size)
{
    FIXME("%p, %p, %u, %p, %u, %p stub.\n", iface, entry, which, buffer, buffer_size, string_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntryStringWide(IDebugSymbols3 *iface,
        DEBUG_SYMBOL_SOURCE_ENTRY *entry, ULONG which, WCHAR *buffer, ULONG buffer_size, ULONG *string_size)
{
    FIXME("%p, %p, %u, %p, %u, %p stub.\n", iface, entry, which, buffer, buffer_size, string_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntryOffsetRegions(IDebugSymbols3 *iface,
        DEBUG_SYMBOL_SOURCE_ENTRY *entry, ULONG flags, DEBUG_OFFSET_REGION *regions, ULONG count, ULONG *regions_avail)
{
    FIXME("%p, %p, %#x, %p, %u, %p stub.\n", iface, entry, flags, regions, count, regions_avail);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugsymbols_GetSourceEntryBySourceEntry(IDebugSymbols3 *iface,
        DEBUG_SYMBOL_SOURCE_ENTRY *from_entry, ULONG flags, DEBUG_SYMBOL_SOURCE_ENTRY *to_entry)
{
    FIXME("%p, %p, %#x, %p stub.\n", iface, from_entry, flags, to_entry);

    return E_NOTIMPL;
}

static const IDebugSymbols3Vtbl debugsymbolsvtbl =
{
    debugsymbols_QueryInterface,
    debugsymbols_AddRef,
    debugsymbols_Release,
    debugsymbols_GetSymbolOptions,
    debugsymbols_AddSymbolOptions,
    debugsymbols_RemoveSymbolOptions,
    debugsymbols_SetSymbolOptions,
    debugsymbols_GetNameByOffset,
    debugsymbols_GetOffsetByName,
    debugsymbols_GetNearNameByOffset,
    debugsymbols_GetLineByOffset,
    debugsymbols_GetOffsetByLine,
    debugsymbols_GetNumberModules,
    debugsymbols_GetModuleByIndex,
    debugsymbols_GetModuleByModuleName,
    debugsymbols_GetModuleByOffset,
    debugsymbols_GetModuleNames,
    debugsymbols_GetModuleParameters,
    debugsymbols_GetSymbolModule,
    debugsymbols_GetTypeName,
    debugsymbols_GetTypeId,
    debugsymbols_GetTypeSize,
    debugsymbols_GetFieldOffset,
    debugsymbols_GetSymbolTypeId,
    debugsymbols_GetOffsetTypeId,
    debugsymbols_ReadTypedDataVirtual,
    debugsymbols_WriteTypedDataVirtual,
    debugsymbols_OutputTypedDataVirtual,
    debugsymbols_ReadTypedDataPhysical,
    debugsymbols_WriteTypedDataPhysical,
    debugsymbols_OutputTypedDataPhysical,
    debugsymbols_GetScope,
    debugsymbols_SetScope,
    debugsymbols_ResetScope,
    debugsymbols_GetScopeSymbolGroup,
    debugsymbols_CreateSymbolGroup,
    debugsymbols_StartSymbolMatch,
    debugsymbols_GetNextSymbolMatch,
    debugsymbols_EndSymbolMatch,
    debugsymbols_Reload,
    debugsymbols_GetSymbolPath,
    debugsymbols_SetSymbolPath,
    debugsymbols_AppendSymbolPath,
    debugsymbols_GetImagePath,
    debugsymbols_SetImagePath,
    debugsymbols_AppendImagePath,
    debugsymbols_GetSourcePath,
    debugsymbols_GetSourcePathElement,
    debugsymbols_SetSourcePath,
    debugsymbols_AppendSourcePath,
    debugsymbols_FindSourceFile,
    debugsymbols_GetSourceFileLineOffsets,
    /* IDebugSymbols2 */
    debugsymbols_GetModuleVersionInformation,
    debugsymbols_GetModuleNameString,
    debugsymbols_GetConstantName,
    debugsymbols_GetFieldName,
    debugsymbols_GetTypeOptions,
    debugsymbols_AddTypeOptions,
    debugsymbols_RemoveTypeOptions,
    debugsymbols_SetTypeOptions,
    /* IDebugSymbols3 */
    debugsymbols_GetNameByOffsetWide,
    debugsymbols_GetOffsetByNameWide,
    debugsymbols_GetNearNameByOffsetWide,
    debugsymbols_GetLineByOffsetWide,
    debugsymbols_GetOffsetByLineWide,
    debugsymbols_GetModuleByModuleNameWide,
    debugsymbols_GetSymbolModuleWide,
    debugsymbols_GetTypeNameWide,
    debugsymbols_GetTypeIdWide,
    debugsymbols_GetFieldOffsetWide,
    debugsymbols_GetSymbolTypeIdWide,
    debugsymbols_GetScopeSymbolGroup2,
    debugsymbols_CreateSymbolGroup2,
    debugsymbols_StartSymbolMatchWide,
    debugsymbols_GetNextSymbolMatchWide,
    debugsymbols_ReloadWide,
    debugsymbols_GetSymbolPathWide,
    debugsymbols_SetSymbolPathWide,
    debugsymbols_AppendSymbolPathWide,
    debugsymbols_GetImagePathWide,
    debugsymbols_SetImagePathWide,
    debugsymbols_AppendImagePathWide,
    debugsymbols_GetSourcePathWide,
    debugsymbols_GetSourcePathElementWide,
    debugsymbols_SetSourcePathWide,
    debugsymbols_AppendSourcePathWide,
    debugsymbols_FindSourceFileWide,
    debugsymbols_GetSourceFileLineOffsetsWide,
    debugsymbols_GetModuleVersionInformationWide,
    debugsymbols_GetModuleNameStringWide,
    debugsymbols_GetConstantNameWide,
    debugsymbols_GetFieldNameWide,
    debugsymbols_IsManagedModule,
    debugsymbols_GetModuleByModuleName2,
    debugsymbols_GetModuleByModuleName2Wide,
    debugsymbols_GetModuleByOffset2,
    debugsymbols_AddSyntheticModule,
    debugsymbols_AddSyntheticModuleWide,
    debugsymbols_RemoveSyntheticModule,
    debugsymbols_GetCurrentScopeFrameIndex,
    debugsymbols_SetScopeFrameByIndex,
    debugsymbols_SetScopeFromJitDebugInfo,
    debugsymbols_SetScopeFromStoredEvent,
    debugsymbols_OutputSymbolByOffset,
    debugsymbols_GetFunctionEntryByOffset,
    debugsymbols_GetFieldTypeAndOffset,
    debugsymbols_GetFieldTypeAndOffsetWide,
    debugsymbols_AddSyntheticSymbol,
    debugsymbols_AddSyntheticSymbolWide,
    debugsymbols_RemoveSyntheticSymbol,
    debugsymbols_GetSymbolEntriesByOffset,
    debugsymbols_GetSymbolEntriesByName,
    debugsymbols_GetSymbolEntriesByNameWide,
    debugsymbols_GetSymbolEntryByToken,
    debugsymbols_GetSymbolEntryInformation,
    debugsymbols_GetSymbolEntryString,
    debugsymbols_GetSymbolEntryStringWide,
    debugsymbols_GetSymbolEntryOffsetRegions,
    debugsymbols_GetSymbolEntryBySymbolEntry,
    debugsymbols_GetSourceEntriesByOffset,
    debugsymbols_GetSourceEntriesByLine,
    debugsymbols_GetSourceEntriesByLineWide,
    debugsymbols_GetSourceEntryString,
    debugsymbols_GetSourceEntryStringWide,
    debugsymbols_GetSourceEntryOffsetRegions,
    debugsymbols_GetSourceEntryBySourceEntry,
};

static HRESULT STDMETHODCALLTYPE debugcontrol_QueryInterface(IDebugControl2 *iface, REFIID riid, void **obj)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_QueryInterface(unk, riid, obj);
}

static ULONG STDMETHODCALLTYPE debugcontrol_AddRef(IDebugControl2 *iface)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_AddRef(unk);
}

static ULONG STDMETHODCALLTYPE debugcontrol_Release(IDebugControl2 *iface)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);
    IUnknown *unk = (IUnknown *)&debug_client->IDebugClient_iface;
    return IUnknown_Release(unk);
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetInterrupt(IDebugControl2 *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetInterrupt(IDebugControl2 *iface, ULONG flags)
{
    FIXME("%p, %#x stub.\n", iface, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetInterruptTimeout(IDebugControl2 *iface, ULONG *timeout)
{
    FIXME("%p, %p stub.\n", iface, timeout);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetInterruptTimeout(IDebugControl2 *iface, ULONG timeout)
{
    FIXME("%p, %u stub.\n", iface, timeout);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetLogFile(IDebugControl2 *iface, char *buffer, ULONG buffer_size,
        ULONG *file_size, BOOL *append)
{
    FIXME("%p, %p, %u, %p, %p stub.\n", iface, buffer, buffer_size, file_size, append);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OpenLogFile(IDebugControl2 *iface, const char *file, BOOL append)
{
    FIXME("%p, %s, %d stub.\n", iface, debugstr_a(file), append);

    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE debugcontrol_CloseLogFile(IDebugControl2 *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE debugcontrol_GetLogMask(IDebugControl2 *iface, ULONG *mask)
{
    FIXME("%p, %p stub.\n", iface, mask);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetLogMask(IDebugControl2 *iface, ULONG mask)
{
    FIXME("%p, %#x stub.\n", iface, mask);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_Input(IDebugControl2 *iface, char *buffer, ULONG buffer_size,
        ULONG *input_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, input_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_ReturnInput(IDebugControl2 *iface, const char *buffer)
{
    FIXME("%p, %s stub.\n", iface, debugstr_a(buffer));

    return E_NOTIMPL;
}

static HRESULT STDMETHODVCALLTYPE debugcontrol_Output(IDebugControl2 *iface, ULONG mask, const char *format, ...)
{
    FIXME("%p, %#x, %s stub.\n", iface, mask, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputVaList(IDebugControl2 *iface, ULONG mask, const char *format,
        __ms_va_list args)
{
    FIXME("%p, %#x, %s stub.\n", iface, mask, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODVCALLTYPE debugcontrol_ControlledOutput(IDebugControl2 *iface, ULONG output_control,
        ULONG mask, const char *format, ...)
{
    FIXME("%p, %u, %#x, %s stub.\n", iface, output_control, mask, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_ControlledOutputVaList(IDebugControl2 *iface, ULONG output_control,
        ULONG mask, const char *format, __ms_va_list args)
{
    FIXME("%p, %u, %#x, %s stub.\n", iface, output_control, mask, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODVCALLTYPE debugcontrol_OutputPrompt(IDebugControl2 *iface, ULONG output_control,
        const char *format, ...)
{
    FIXME("%p, %u, %s stub.\n", iface, output_control, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputPromptVaList(IDebugControl2 *iface, ULONG output_control,
        const char *format, __ms_va_list args)
{
    FIXME("%p, %u, %s stub.\n", iface, output_control, debugstr_a(format));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetPromptText(IDebugControl2 *iface, char *buffer, ULONG buffer_size,
        ULONG *text_size)
{
    FIXME("%p, %p, %u, %p stub.\n", iface, buffer, buffer_size, text_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputCurrentState(IDebugControl2 *iface, ULONG output_control,
        ULONG flags)
{
    FIXME("%p, %u, %#x stub.\n", iface, output_control, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputVersionInformation(IDebugControl2 *iface, ULONG output_control)
{
    FIXME("%p, %u stub.\n", iface, output_control);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNotifyEventHandle(IDebugControl2 *iface, ULONG64 *handle)
{
    FIXME("%p, %p stub.\n", iface, handle);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetNotifyEventHandle(IDebugControl2 *iface, ULONG64 handle)
{
    FIXME("%p, %s stub.\n", iface, wine_dbgstr_longlong(handle));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_Assemble(IDebugControl2 *iface, ULONG64 offset, const char *code,
        ULONG64 *end_offset)
{
    FIXME("%p, %s, %s, %p stub.\n", iface, wine_dbgstr_longlong(offset), debugstr_a(code), end_offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_Disassemble(IDebugControl2 *iface, ULONG64 offset, ULONG flags,
        char *buffer, ULONG buffer_size, ULONG *disassm_size, ULONG64 *end_offset)
{
    FIXME("%p, %s, %#x, %p, %u, %p, %p stub.\n", iface, wine_dbgstr_longlong(offset), flags, buffer, buffer_size,
            disassm_size, end_offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetDisassembleEffectiveOffset(IDebugControl2 *iface, ULONG64 *offset)
{
    FIXME("%p, %p stub.\n", iface, offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputDisassembly(IDebugControl2 *iface, ULONG output_control,
        ULONG64 offset, ULONG flags, ULONG64 *end_offset)
{
    FIXME("%p, %u, %s, %#x, %p stub.\n", iface, output_control, wine_dbgstr_longlong(offset), flags, end_offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputDisassemblyLines(IDebugControl2 *iface, ULONG output_control,
        ULONG prev_lines, ULONG total_lines, ULONG64 offset, ULONG flags, ULONG *offset_line, ULONG64 *start_offset,
        ULONG64 *end_offset, ULONG64 *line_offsets)
{
    FIXME("%p, %u, %u, %u, %s, %#x, %p, %p, %p, %p stub.\n", iface, output_control, prev_lines, total_lines,
            wine_dbgstr_longlong(offset), flags, offset_line, start_offset, end_offset, line_offsets);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNearInstruction(IDebugControl2 *iface, ULONG64 offset, LONG delta,
        ULONG64 *instr_offset)
{
    FIXME("%p, %s, %d, %p stub.\n", iface, wine_dbgstr_longlong(offset), delta, instr_offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetStackTrace(IDebugControl2 *iface, ULONG64 frame_offset,
        ULONG64 stack_offset, ULONG64 instr_offset, DEBUG_STACK_FRAME *frames, ULONG frames_size, ULONG *frames_filled)
{
    FIXME("%p, %s, %s, %s, %p, %u, %p stub.\n", iface, wine_dbgstr_longlong(frame_offset),
            wine_dbgstr_longlong(stack_offset), wine_dbgstr_longlong(instr_offset), frames, frames_size, frames_filled);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetReturnOffset(IDebugControl2 *iface, ULONG64 *offset)
{
    FIXME("%p, %p stub.\n", iface, offset);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputStackTrace(IDebugControl2 *iface, ULONG output_control,
        DEBUG_STACK_FRAME *frames, ULONG frames_size, ULONG flags)
{
    FIXME("%p, %u, %p, %u, %#x stub.\n", iface, output_control, frames, frames_size, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetDebuggeeType(IDebugControl2 *iface, ULONG *_class, ULONG *qualifier)
{
    FIXME("%p, %p, %p stub.\n", iface, _class, qualifier);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetActualProcessorType(IDebugControl2 *iface, ULONG *type)
{
    FIXME("%p, %p stub.\n", iface, type);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetExecutingProcessorType(IDebugControl2 *iface, ULONG *type)
{
    FIXME("%p, %p stub.\n", iface, type);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberPossibleExecutingProcessorTypes(IDebugControl2 *iface,
        ULONG *count)
{
    FIXME("%p, %p stub.\n", iface, count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetPossibleExecutingProcessorTypes(IDebugControl2 *iface, ULONG start,
        ULONG count, ULONG *types)
{
    FIXME("%p, %u, %u, %p stub.\n", iface, start, count, types);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberProcessors(IDebugControl2 *iface, ULONG *count)
{
    FIXME("%p, %p stub.\n", iface, count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetSystemVersion(IDebugControl2 *iface, ULONG *platform_id, ULONG *major,
        ULONG *minor, char *sp_string, ULONG sp_string_size, ULONG *sp_string_used, ULONG *sp_number,
        char *build_string, ULONG build_string_size, ULONG *build_string_used)
{
    FIXME("%p, %p, %p, %p, %p, %u, %p, %p, %p, %u, %p stub.\n", iface, platform_id, major, minor, sp_string,
            sp_string_size, sp_string_used, sp_number, build_string, build_string_size, build_string_used);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetPageSize(IDebugControl2 *iface, ULONG *size)
{
    FIXME("%p, %p stub.\n", iface, size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_IsPointer64Bit(IDebugControl2 *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_ReadBugCheckData(IDebugControl2 *iface, ULONG *code, ULONG64 *arg1,
        ULONG64 *arg2, ULONG64 *arg3, ULONG64 *arg4)
{
    FIXME("%p, %p, %p, %p, %p, %p stub.\n", iface, code, arg1, arg2, arg3, arg4);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberSupportedProcessorTypes(IDebugControl2 *iface, ULONG *count)
{
    FIXME("%p, %p stub.\n", iface, count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetSupportedProcessorTypes(IDebugControl2 *iface, ULONG start,
        ULONG count, ULONG *types)
{
    FIXME("%p, %u, %u, %p stub.\n", iface, start, count, types);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetProcessorTypeNames(IDebugControl2 *iface, ULONG type, char *full_name,
        ULONG full_name_buffer_size, ULONG *full_name_size, char *abbrev_name, ULONG abbrev_name_buffer_size,
        ULONG *abbrev_name_size)
{
    FIXME("%p, %u, %p, %u, %p, %p, %u, %p stub.\n", iface, type, full_name, full_name_buffer_size, full_name_size,
            abbrev_name, abbrev_name_buffer_size, abbrev_name_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetEffectiveProcessorType(IDebugControl2 *iface, ULONG *type)
{
    FIXME("%p, %p stub.\n", iface, type);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetEffectiveProcessorType(IDebugControl2 *iface, ULONG type)
{
    FIXME("%p, %u stub.\n", iface, type);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetExecutionStatus(IDebugControl2 *iface, ULONG *status)
{
    FIXME("%p, %p stub.\n", iface, status);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetExecutionStatus(IDebugControl2 *iface, ULONG status)
{
    FIXME("%p, %u stub.\n", iface, status);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetCodeLevel(IDebugControl2 *iface, ULONG *level)
{
    FIXME("%p, %p stub.\n", iface, level);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetCodeLevel(IDebugControl2 *iface, ULONG level)
{
    FIXME("%p, %u stub.\n", iface, level);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetEngineOptions(IDebugControl2 *iface, ULONG *options)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);

    TRACE("%p, %p.\n", iface, options);

    *options = debug_client->engine_options;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_AddEngineOptions(IDebugControl2 *iface, ULONG options)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);

    TRACE("%p, %#x.\n", iface, options);

    if (options & ~DEBUG_ENGOPT_ALL)
        return E_INVALIDARG;

    debug_client->engine_options |= options;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_RemoveEngineOptions(IDebugControl2 *iface, ULONG options)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);

    TRACE("%p, %#x.\n", iface, options);

    debug_client->engine_options &= ~options;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetEngineOptions(IDebugControl2 *iface, ULONG options)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);

    TRACE("%p, %#x.\n", iface, options);

    if (options & ~DEBUG_ENGOPT_ALL)
        return E_INVALIDARG;

    debug_client->engine_options = options;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetSystemErrorControl(IDebugControl2 *iface, ULONG *output_level,
        ULONG *break_level)
{
    FIXME("%p, %p, %p stub.\n", iface, output_level, break_level);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetSystemErrorControl(IDebugControl2 *iface, ULONG output_level,
        ULONG break_level)
{
    FIXME("%p, %u, %u stub.\n", iface, output_level, break_level);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetTextMacro(IDebugControl2 *iface, ULONG slot, char *buffer,
        ULONG buffer_size, ULONG *macro_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, slot, buffer, buffer_size, macro_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetTextMacro(IDebugControl2 *iface, ULONG slot, const char *macro)
{
    FIXME("%p, %u, %s stub.\n", iface, slot, debugstr_a(macro));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetRadix(IDebugControl2 *iface, ULONG *radix)
{
    FIXME("%p, %p stub.\n", iface, radix);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetRadix(IDebugControl2 *iface, ULONG radix)
{
    FIXME("%p, %u stub.\n", iface, radix);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_Evaluate(IDebugControl2 *iface, const char *expression,
        ULONG desired_type, DEBUG_VALUE *value, ULONG *remainder_index)
{
    FIXME("%p, %s, %u, %p, %p stub.\n", iface, debugstr_a(expression), desired_type, value, remainder_index);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_CoerceValue(IDebugControl2 *iface, DEBUG_VALUE input, ULONG output_type,
        DEBUG_VALUE *output)
{
    FIXME("%p, %u, %p stub.\n", iface, output_type, output);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_CoerceValues(IDebugControl2 *iface, ULONG count, DEBUG_VALUE *input,
        ULONG *output_types, DEBUG_VALUE *output)
{
    FIXME("%p, %u, %p, %p, %p stub.\n", iface, count, input, output_types, output);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_Execute(IDebugControl2 *iface, ULONG output_control, const char *command,
        ULONG flags)
{
    FIXME("%p, %u, %s, %#x stub.\n", iface, output_control, debugstr_a(command), flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_ExecuteCommandFile(IDebugControl2 *iface, ULONG output_control,
        const char *command_file, ULONG flags)
{
    FIXME("%p, %u, %s, %#x stub.\n", iface, output_control, debugstr_a(command_file), flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberBreakpoints(IDebugControl2 *iface, ULONG *count)
{
    FIXME("%p, %p stub.\n", iface, count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetBreakpointByIndex(IDebugControl2 *iface, ULONG index,
        IDebugBreakpoint **bp)
{
    FIXME("%p, %u, %p stub.\n", iface, index, bp);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetBreakpointById(IDebugControl2 *iface, ULONG id, IDebugBreakpoint **bp)
{
    FIXME("%p, %u, %p stub.\n", iface, id, bp);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetBreakpointParameters(IDebugControl2 *iface, ULONG count, ULONG *ids,
        ULONG start, DEBUG_BREAKPOINT_PARAMETERS *parameters)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, count, ids, start, parameters);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_AddBreakpoint(IDebugControl2 *iface, ULONG type, ULONG desired_id,
        IDebugBreakpoint **bp)
{
    FIXME("%p, %u, %u, %p stub.\n", iface, type, desired_id, bp);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_RemoveBreakpoint(IDebugControl2 *iface, IDebugBreakpoint *bp)
{
    FIXME("%p, %p stub.\n", iface, bp);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_AddExtension(IDebugControl2 *iface, const char *path, ULONG flags,
        ULONG64 *handle)
{
    FIXME("%p, %s, %#x, %p stub.\n", iface, debugstr_a(path), flags, handle);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_RemoveExtension(IDebugControl2 *iface, ULONG64 handle)
{
    FIXME("%p, %s stub.\n", iface, wine_dbgstr_longlong(handle));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetExtensionByPath(IDebugControl2 *iface, const char *path,
        ULONG64 *handle)
{
    FIXME("%p, %s, %p stub.\n", iface, debugstr_a(path), handle);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_CallExtension(IDebugControl2 *iface, ULONG64 handle,
        const char *function, const char *args)
{
    FIXME("%p, %s, %s, %s stub.\n", iface, wine_dbgstr_longlong(handle), debugstr_a(function), debugstr_a(args));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetExtensionFunction(IDebugControl2 *iface, ULONG64 handle,
        const char *name, void *function)
{
    FIXME("%p, %s, %s, %p stub.\n", iface, wine_dbgstr_longlong(handle), debugstr_a(name), function);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetWindbgExtensionApis32(IDebugControl2 *iface,
        PWINDBG_EXTENSION_APIS32 api)
{
    FIXME("%p, %p stub.\n", iface, api);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetWindbgExtensionApis64(IDebugControl2 *iface,
        PWINDBG_EXTENSION_APIS64 api)
{
    FIXME("%p, %p stub.\n", iface, api);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberEventFilters(IDebugControl2 *iface, ULONG *specific_events,
        ULONG *specific_exceptions, ULONG *arbitrary_exceptions)
{
    FIXME("%p, %p, %p, %p stub.\n", iface, specific_events, specific_exceptions, arbitrary_exceptions);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetEventFilterText(IDebugControl2 *iface, ULONG index, char *buffer,
        ULONG buffer_size, ULONG *text_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, text_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetEventFilterCommand(IDebugControl2 *iface, ULONG index, char *buffer,
        ULONG buffer_size, ULONG *command_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, command_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetEventFilterCommand(IDebugControl2 *iface, ULONG index,
        const char *command)
{
    FIXME("%p, %u, %s stub.\n", iface, index, debugstr_a(command));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetSpecificFilterParameters(IDebugControl2 *iface, ULONG start,
        ULONG count, DEBUG_SPECIFIC_FILTER_PARAMETERS *parameters)
{
    FIXME("%p, %u, %u, %p stub.\n", iface, start, count, parameters);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetSpecificFilterParameters(IDebugControl2 *iface, ULONG start,
        ULONG count, DEBUG_SPECIFIC_FILTER_PARAMETERS *parameters)
{
    FIXME("%p, %u, %u, %p stub.\n", iface, start, count, parameters);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetSpecificFilterArgument(IDebugControl2 *iface, ULONG index,
        char *buffer, ULONG buffer_size, ULONG *argument_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, argument_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetSpecificFilterArgument(IDebugControl2 *iface, ULONG index,
        const char *argument)
{
    FIXME("%p, %u, %s stub.\n", iface, index, debugstr_a(argument));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetExceptionFilterParameters(IDebugControl2 *iface, ULONG count,
        ULONG *codes, ULONG start, DEBUG_EXCEPTION_FILTER_PARAMETERS *parameters)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, count, codes, start, parameters);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetExceptionFilterParameters(IDebugControl2 *iface, ULONG count,
        DEBUG_EXCEPTION_FILTER_PARAMETERS *parameters)
{
    FIXME("%p, %u, %p stub.\n", iface, count, parameters);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetExceptionFilterSecondCommand(IDebugControl2 *iface, ULONG index,
        char *buffer, ULONG buffer_size, ULONG *command_size)
{
    FIXME("%p, %u, %p, %u, %p stub.\n", iface, index, buffer, buffer_size, command_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetExceptionFilterSecondCommand(IDebugControl2 *iface, ULONG index,
        const char *command)
{
    FIXME("%p, %u, %s stub.\n", iface, index, debugstr_a(command));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_WaitForEvent(IDebugControl2 *iface, ULONG flags, ULONG timeout)
{
    struct debug_client *debug_client = impl_from_IDebugControl2(iface);
    struct target_process *target;

    TRACE("%p, %#x, %u.\n", iface, flags, timeout);

    /* FIXME: only one target is used currently */

    if (!(target = debug_client_get_target(debug_client)))
        return E_UNEXPECTED;

    if (target->attach_flags & DEBUG_ATTACH_NONINVASIVE)
    {
        BOOL suspend = !(target->attach_flags & DEBUG_ATTACH_NONINVASIVE_NO_SUSPEND);
        DWORD access = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_LIMITED_INFORMATION;
        NTSTATUS status;

        if (suspend)
            access |= PROCESS_SUSPEND_RESUME;

        target->handle = OpenProcess(access, FALSE, target->pid);
        if (!target->handle)
        {
            WARN("Failed to get process handle for pid %#x.\n", target->pid);
            return E_UNEXPECTED;
        }

        if (suspend)
        {
            status = NtSuspendProcess(target->handle);
            if (status)
                WARN("Failed to suspend a process, status %#x.\n", status);
        }

        return S_OK;
    }
    else
    {
        FIXME("Unsupported attach flags %#x.\n", target->attach_flags);
    }

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetLastEventInformation(IDebugControl2 *iface, ULONG *type, ULONG *pid,
        ULONG *tid, void *extra_info, ULONG extra_info_size, ULONG *extra_info_used, char *description,
        ULONG desc_size, ULONG *desc_used)
{
    FIXME("%p, %p, %p, %p, %p, %u, %p, %p, %u, %p stub.\n", iface, type, pid, tid, extra_info, extra_info_size,
            extra_info_used, description, desc_size, desc_used);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetCurrentTimeDate(IDebugControl2 *iface, ULONG timedate)
{
    FIXME("%p, %u stub.\n", iface, timedate);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetCurrentSystemUpTime(IDebugControl2 *iface, ULONG uptime)
{
    FIXME("%p, %u stub.\n", iface, uptime);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetDumpFormatFlags(IDebugControl2 *iface, ULONG *flags)
{
    FIXME("%p, %p stub.\n", iface, flags);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberTextPlacements(IDebugControl2 *iface, ULONG *count)
{
    FIXME("%p, %p stub.\n", iface, count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_GetNumberTextReplacement(IDebugControl2 *iface, const char *src_text,
        ULONG index, char *src_buffer, ULONG src_buffer_size, ULONG *src_size, char *dst_buffer,
        ULONG dst_buffer_size, ULONG *dst_size)
{
    FIXME("%p, %s, %u, %p, %u, %p, %p, %u, %p stub.\n", iface, debugstr_a(src_text), index, src_buffer,
            src_buffer_size, src_size, dst_buffer, dst_buffer_size, dst_size);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_SetTextReplacement(IDebugControl2 *iface, const char *src_text,
        const char *dst_text)
{
    FIXME("%p, %s, %s stub.\n", iface, debugstr_a(src_text), debugstr_a(dst_text));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_RemoveTextReplacements(IDebugControl2 *iface)
{
    FIXME("%p stub.\n", iface);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE debugcontrol_OutputTextReplacements(IDebugControl2 *iface, ULONG output_control,
        ULONG flags)
{
    FIXME("%p, %u, %#x stub.\n", iface, output_control, flags);

    return E_NOTIMPL;
}

static const IDebugControl2Vtbl debugcontrolvtbl =
{
    debugcontrol_QueryInterface,
    debugcontrol_AddRef,
    debugcontrol_Release,
    debugcontrol_GetInterrupt,
    debugcontrol_SetInterrupt,
    debugcontrol_GetInterruptTimeout,
    debugcontrol_SetInterruptTimeout,
    debugcontrol_GetLogFile,
    debugcontrol_OpenLogFile,
    debugcontrol_CloseLogFile,
    debugcontrol_GetLogMask,
    debugcontrol_SetLogMask,
    debugcontrol_Input,
    debugcontrol_ReturnInput,
    debugcontrol_Output,
    debugcontrol_OutputVaList,
    debugcontrol_ControlledOutput,
    debugcontrol_ControlledOutputVaList,
    debugcontrol_OutputPrompt,
    debugcontrol_OutputPromptVaList,
    debugcontrol_GetPromptText,
    debugcontrol_OutputCurrentState,
    debugcontrol_OutputVersionInformation,
    debugcontrol_GetNotifyEventHandle,
    debugcontrol_SetNotifyEventHandle,
    debugcontrol_Assemble,
    debugcontrol_Disassemble,
    debugcontrol_GetDisassembleEffectiveOffset,
    debugcontrol_OutputDisassembly,
    debugcontrol_OutputDisassemblyLines,
    debugcontrol_GetNearInstruction,
    debugcontrol_GetStackTrace,
    debugcontrol_GetReturnOffset,
    debugcontrol_OutputStackTrace,
    debugcontrol_GetDebuggeeType,
    debugcontrol_GetActualProcessorType,
    debugcontrol_GetExecutingProcessorType,
    debugcontrol_GetNumberPossibleExecutingProcessorTypes,
    debugcontrol_GetPossibleExecutingProcessorTypes,
    debugcontrol_GetNumberProcessors,
    debugcontrol_GetSystemVersion,
    debugcontrol_GetPageSize,
    debugcontrol_IsPointer64Bit,
    debugcontrol_ReadBugCheckData,
    debugcontrol_GetNumberSupportedProcessorTypes,
    debugcontrol_GetSupportedProcessorTypes,
    debugcontrol_GetProcessorTypeNames,
    debugcontrol_GetEffectiveProcessorType,
    debugcontrol_SetEffectiveProcessorType,
    debugcontrol_GetExecutionStatus,
    debugcontrol_SetExecutionStatus,
    debugcontrol_GetCodeLevel,
    debugcontrol_SetCodeLevel,
    debugcontrol_GetEngineOptions,
    debugcontrol_AddEngineOptions,
    debugcontrol_RemoveEngineOptions,
    debugcontrol_SetEngineOptions,
    debugcontrol_GetSystemErrorControl,
    debugcontrol_SetSystemErrorControl,
    debugcontrol_GetTextMacro,
    debugcontrol_SetTextMacro,
    debugcontrol_GetRadix,
    debugcontrol_SetRadix,
    debugcontrol_Evaluate,
    debugcontrol_CoerceValue,
    debugcontrol_CoerceValues,
    debugcontrol_Execute,
    debugcontrol_ExecuteCommandFile,
    debugcontrol_GetNumberBreakpoints,
    debugcontrol_GetBreakpointByIndex,
    debugcontrol_GetBreakpointById,
    debugcontrol_GetBreakpointParameters,
    debugcontrol_AddBreakpoint,
    debugcontrol_RemoveBreakpoint,
    debugcontrol_AddExtension,
    debugcontrol_RemoveExtension,
    debugcontrol_GetExtensionByPath,
    debugcontrol_CallExtension,
    debugcontrol_GetExtensionFunction,
    debugcontrol_GetWindbgExtensionApis32,
    debugcontrol_GetWindbgExtensionApis64,
    debugcontrol_GetNumberEventFilters,
    debugcontrol_GetEventFilterText,
    debugcontrol_GetEventFilterCommand,
    debugcontrol_SetEventFilterCommand,
    debugcontrol_GetSpecificFilterParameters,
    debugcontrol_SetSpecificFilterParameters,
    debugcontrol_GetSpecificFilterArgument,
    debugcontrol_SetSpecificFilterArgument,
    debugcontrol_GetExceptionFilterParameters,
    debugcontrol_SetExceptionFilterParameters,
    debugcontrol_GetExceptionFilterSecondCommand,
    debugcontrol_SetExceptionFilterSecondCommand,
    debugcontrol_WaitForEvent,
    debugcontrol_GetLastEventInformation,
    debugcontrol_GetCurrentTimeDate,
    debugcontrol_GetCurrentSystemUpTime,
    debugcontrol_GetDumpFormatFlags,
    debugcontrol_GetNumberTextPlacements,
    debugcontrol_GetNumberTextReplacement,
    debugcontrol_SetTextReplacement,
    debugcontrol_RemoveTextReplacements,
    debugcontrol_OutputTextReplacements,
};

/************************************************************
*                    DebugExtensionInitialize   (DBGENG.@)
*
* Initializing Debug Engine
*
* PARAMS
*   pVersion  [O] Receiving the version of extension
*   pFlags    [O] Reserved
*
* RETURNS
*   Success: S_OK
*   Failure: Anything other than S_OK
*
* BUGS
*   Unimplemented
*/
HRESULT WINAPI DebugExtensionInitialize(ULONG * pVersion, ULONG * pFlags)
{
    FIXME("(%p,%p): stub\n", pVersion, pFlags);

    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);

    return E_NOTIMPL;
}

/************************************************************
*                    DebugCreate   (dbgeng.@)
*/
HRESULT WINAPI DebugCreate(REFIID riid, void **obj)
{
    struct debug_client *debug_client;
    IUnknown *unk;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    debug_client = heap_alloc_zero(sizeof(*debug_client));
    if (!debug_client)
        return E_OUTOFMEMORY;

    debug_client->IDebugClient_iface.lpVtbl = &debugclientvtbl;
    debug_client->IDebugDataSpaces_iface.lpVtbl = &debugdataspacesvtbl;
    debug_client->IDebugSymbols3_iface.lpVtbl = &debugsymbolsvtbl;
    debug_client->IDebugControl2_iface.lpVtbl = &debugcontrolvtbl;
    debug_client->refcount = 1;
    list_init(&debug_client->targets);

    unk = (IUnknown *)&debug_client->IDebugClient_iface;

    hr = IUnknown_QueryInterface(unk, riid, obj);
    IUnknown_Release(unk);

    return hr;
}

/************************************************************
*                    DebugCreateEx   (DBGENG.@)
*/
HRESULT WINAPI DebugCreateEx(REFIID riid, DWORD flags, void **obj)
{
    FIXME("(%s, %#x, %p): stub\n", debugstr_guid(riid), flags, obj);

    return E_NOTIMPL;
}

/************************************************************
*                    DebugConnect   (DBGENG.@)
*
* Creating Debug Engine client object and connecting it to remote host
*
* PARAMS
*   RemoteOptions [I] Options which define how debugger engine connects to remote host
*   InterfaceId   [I] Interface Id of debugger client
*   pInterface    [O] Pointer to interface as requested via InterfaceId
*
* RETURNS
*   Success: S_OK
*   Failure: Anything other than S_OK
*
* BUGS
*   Unimplemented
*/
HRESULT WINAPI DebugConnect(PCSTR RemoteOptions, REFIID InterfaceId, PVOID * pInterface)
{
    FIXME("(%p,%p,%p): stub\n", RemoteOptions, InterfaceId, pInterface);

    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);

    return E_NOTIMPL;
}
