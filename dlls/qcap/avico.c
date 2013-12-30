/*
 * Copyright 2013 Jacek Caban for CodeWeavers
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
#include "dshow.h"

#include "qcap_main.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(qcap);

typedef struct {
    BaseFilter filter;
    IPersistPropertyBag IPersistPropertyBag_iface;

    BaseInputPin *in;
    BaseOutputPin *out;
} AVICompressor;

static inline AVICompressor *impl_from_BaseFilter(BaseFilter *filter)
{
    return CONTAINING_RECORD(filter, AVICompressor, filter);
}

static inline AVICompressor *impl_from_IBaseFilter(IBaseFilter *iface)
{
    BaseFilter *filter = CONTAINING_RECORD(iface, BaseFilter, IBaseFilter_iface);
    return impl_from_BaseFilter(filter);
}

static inline AVICompressor *impl_from_BasePin(BasePin *pin)
{
    return impl_from_IBaseFilter(pin->pinInfo.pFilter);
}

static HRESULT WINAPI AVICompressor_QueryInterface(IBaseFilter *iface, REFIID riid, void **ppv)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);

    if(IsEqualIID(riid, &IID_IUnknown)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", This, ppv);
        *ppv = &This->filter.IBaseFilter_iface;
    }else if(IsEqualIID(riid, &IID_IPersist)) {
        TRACE("(%p)->(IID_IPersist %p)\n", This, ppv);
        *ppv = &This->filter.IBaseFilter_iface;
    }else if(IsEqualIID(riid, &IID_IMediaFilter)) {
        TRACE("(%p)->(IID_IMediaFilter %p)\n", This, ppv);
        *ppv = &This->filter.IBaseFilter_iface;
    }else if(IsEqualIID(riid, &IID_IBaseFilter)) {
        TRACE("(%p)->(IID_IBaseFilter %p)\n", This, ppv);
        *ppv = &This->filter.IBaseFilter_iface;
    }else if(IsEqualIID(riid, &IID_IPersistPropertyBag)) {
        TRACE("(%p)->(IID_IPersistPropertyBag %p)\n", This, ppv);
        *ppv = &This->IPersistPropertyBag_iface;
    }else {
        FIXME("no interface for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;

}

static ULONG WINAPI AVICompressor_Release(IBaseFilter *iface)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    ULONG ref = BaseFilterImpl_Release(&This->filter.IBaseFilter_iface);

    TRACE("(%p) ref=%d\n", This, ref);

    if(!ref) {
        if(This->in)
            BaseInputPinImpl_Release(&This->in->pin.IPin_iface);
        if(This->out)
            BaseOutputPinImpl_Release(&This->out->pin.IPin_iface);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI AVICompressor_Stop(IBaseFilter *iface)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressor_Pause(IBaseFilter *iface)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressor_Run(IBaseFilter *iface, REFERENCE_TIME tStart)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    FIXME("(%p)->(%s)\n", This, wine_dbgstr_longlong(tStart));
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressor_FindPin(IBaseFilter *iface, LPCWSTR Id, IPin **ppPin)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    FIXME("(%p)->(%s %p)\n", This, debugstr_w(Id), ppPin);
    return VFW_E_NOT_FOUND;
}

static HRESULT WINAPI AVICompressor_QueryFilterInfo(IBaseFilter *iface, FILTER_INFO *pInfo)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    FIXME("(%p)->(%p)\n", This, pInfo);
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressor_QueryVendorInfo(IBaseFilter *iface, LPWSTR *pVendorInfo)
{
    AVICompressor *This = impl_from_IBaseFilter(iface);
    FIXME("(%p)->(%p)\n", This, pVendorInfo);
    return E_NOTIMPL;
}

static const IBaseFilterVtbl AVICompressorVtbl = {
    AVICompressor_QueryInterface,
    BaseFilterImpl_AddRef,
    AVICompressor_Release,
    BaseFilterImpl_GetClassID,
    AVICompressor_Stop,
    AVICompressor_Pause,
    AVICompressor_Run,
    BaseFilterImpl_GetState,
    BaseFilterImpl_SetSyncSource,
    BaseFilterImpl_GetSyncSource,
    BaseFilterImpl_EnumPins,
    AVICompressor_FindPin,
    AVICompressor_QueryFilterInfo,
    BaseFilterImpl_JoinFilterGraph,
    AVICompressor_QueryVendorInfo
};

static IPin* WINAPI AVICompressor_GetPin(BaseFilter *iface, int pos)
{
    AVICompressor *This = impl_from_BaseFilter(iface);
    IPin *ret;

    TRACE("(%p)->(%d)\n", This, pos);

    switch(pos) {
    case 0:
        ret = &This->in->pin.IPin_iface;
        break;
    case 1:
        ret = &This->out->pin.IPin_iface;
        break;
    default:
        TRACE("No pin %d\n", pos);
        return NULL;
    };

    IPin_AddRef(ret);
    return ret;
}

static LONG WINAPI AVICompressor_GetPinCount(BaseFilter *iface)
{
    return 2;
}

static const BaseFilterFuncTable filter_func_table = {
    AVICompressor_GetPin,
    AVICompressor_GetPinCount
};

static AVICompressor *impl_from_IPersistPropertyBag(IPersistPropertyBag *iface)
{
    return CONTAINING_RECORD(iface, AVICompressor, IPersistPropertyBag_iface);
}

static HRESULT WINAPI AVICompressorPropertyBag_QueryInterface(IPersistPropertyBag *iface, REFIID riid, void **ppv)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    return IBaseFilter_QueryInterface(&This->filter.IBaseFilter_iface, riid, ppv);
}

static ULONG WINAPI AVICompressorPropertyBag_AddRef(IPersistPropertyBag *iface)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    return IBaseFilter_AddRef(&This->filter.IBaseFilter_iface);
}

static ULONG WINAPI AVICompressorPropertyBag_Release(IPersistPropertyBag *iface)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    return IBaseFilter_Release(&This->filter.IBaseFilter_iface);
}

static HRESULT WINAPI AVICompressorPropertyBag_GetClassID(IPersistPropertyBag *iface, CLSID *pClassID)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    return IBaseFilter_GetClassID(&This->filter.IBaseFilter_iface, pClassID);
}

static HRESULT WINAPI AVICompressorPropertyBag_InitNew(IPersistPropertyBag *iface)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressorPropertyBag_Load(IPersistPropertyBag *iface, IPropertyBag *pPropBag, IErrorLog *pErrorLog)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    FIXME("(%p)->(%p %p)\n", This, pPropBag, pErrorLog);
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressorPropertyBag_Save(IPersistPropertyBag *iface, IPropertyBag *pPropBag,
        BOOL fClearDirty, BOOL fSaveAllProperties)
{
    AVICompressor *This = impl_from_IPersistPropertyBag(iface);
    FIXME("(%p)->(%p %x %x)\n", This, pPropBag, fClearDirty, fSaveAllProperties);
    return E_NOTIMPL;
}

static const IPersistPropertyBagVtbl PersistPropertyBagVtbl = {
    AVICompressorPropertyBag_QueryInterface,
    AVICompressorPropertyBag_AddRef,
    AVICompressorPropertyBag_Release,
    AVICompressorPropertyBag_GetClassID,
    AVICompressorPropertyBag_InitNew,
    AVICompressorPropertyBag_Load,
    AVICompressorPropertyBag_Save
};

static inline AVICompressor *impl_from_IPin(IPin *iface)
{
    BasePin *bp = CONTAINING_RECORD(iface, BasePin, IPin_iface);
    return impl_from_IBaseFilter(bp->pinInfo.pFilter);
}

static HRESULT WINAPI AVICompressorIn_QueryInterface(IPin *iface, REFIID riid, void **ppv)
{
    return BaseInputPinImpl_QueryInterface(iface, riid, ppv);
}

static ULONG WINAPI AVICompressorIn_AddRef(IPin *iface)
{
    AVICompressor *This = impl_from_IPin(iface);
    return IBaseFilter_AddRef(&This->filter.IBaseFilter_iface);
}

static ULONG WINAPI AVICompressorIn_Release(IPin *iface)
{
    AVICompressor *This = impl_from_IPin(iface);
    return IBaseFilter_Release(&This->filter.IBaseFilter_iface);
}

static HRESULT WINAPI AVICompressorIn_ReceiveConnection(IPin *iface,
        IPin *pConnector, const AM_MEDIA_TYPE *pmt)
{
    AVICompressor *This = impl_from_IPin(iface);
    FIXME("(%p)->(%p AM_MEDIA_TYPE(%p))\n", This, pConnector, pmt);
    dump_AM_MEDIA_TYPE(pmt);
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressorIn_Disconnect(IPin *iface)
{
    AVICompressor *This = impl_from_IPin(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static const IPinVtbl AVICompressorInputPinVtbl = {
    AVICompressorIn_QueryInterface,
    AVICompressorIn_AddRef,
    AVICompressorIn_Release,
    BaseInputPinImpl_Connect,
    AVICompressorIn_ReceiveConnection,
    AVICompressorIn_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BasePinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    BaseInputPinImpl_EndOfStream,
    BaseInputPinImpl_BeginFlush,
    BaseInputPinImpl_EndFlush,
    BaseInputPinImpl_NewSegment
};

static HRESULT WINAPI AVICompressorIn_CheckMediaType(BasePin *base, const AM_MEDIA_TYPE *pmt)
{
    FIXME("(%p)->(AM_MEDIA_TYPE(%p))\n", base, pmt);
    dump_AM_MEDIA_TYPE(pmt);
    return E_NOTIMPL;
}

static LONG WINAPI AVICompressorIn_GetMediaTypeVersion(BasePin *base)
{
    return 0;
}

static HRESULT WINAPI AVICompressorIn_GetMediaType(BasePin *base, int iPosition, AM_MEDIA_TYPE *amt)
{
    TRACE("(%p)->(%d %p)\n", base, iPosition, amt);
    return S_FALSE;
}

static const BasePinFuncTable AVICompressorInputBasePinVtbl = {
    AVICompressorIn_CheckMediaType,
    NULL,
    AVICompressorIn_GetMediaTypeVersion,
    AVICompressorIn_GetMediaType
};

static HRESULT WINAPI AVICompressorIn_Receive(BaseInputPin *base, IMediaSample *pSample)
{
    AVICompressor *This = impl_from_BasePin(&base->pin);
    FIXME("(%p)->(%p)\n", This, pSample);
    return E_NOTIMPL;
}

static const BaseInputPinFuncTable AVICompressorBaseInputPinVtbl = {
    AVICompressorIn_Receive
};

static HRESULT WINAPI AVICompressorOut_QueryInterface(IPin *iface, REFIID riid, void **ppv)
{
    return BaseInputPinImpl_QueryInterface(iface, riid, ppv);
}

static ULONG WINAPI AVICompressorOut_AddRef(IPin *iface)
{
    AVICompressor *This = impl_from_IPin(iface);
    return IBaseFilter_AddRef(&This->filter.IBaseFilter_iface);
}

static ULONG WINAPI AVICompressorOut_Release(IPin *iface)
{
    AVICompressor *This = impl_from_IPin(iface);
    return IBaseFilter_Release(&This->filter.IBaseFilter_iface);
}

static const IPinVtbl AVICompressorOutputPinVtbl = {
    AVICompressorOut_QueryInterface,
    AVICompressorOut_AddRef,
    AVICompressorOut_Release,
    BaseOutputPinImpl_Connect,
    BaseOutputPinImpl_ReceiveConnection,
    BaseOutputPinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BasePinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    BaseOutputPinImpl_EndOfStream,
    BaseOutputPinImpl_BeginFlush,
    BaseOutputPinImpl_EndFlush,
    BasePinImpl_NewSegment
};

static LONG WINAPI AVICompressorOut_GetMediaTypeVersion(BasePin *base)
{
    FIXME("(%p)\n", base);
    return 0;
}

static HRESULT WINAPI AVICompressorOut_GetMediaType(BasePin *base, int iPosition, AM_MEDIA_TYPE *amt)
{
    AVICompressor *This = impl_from_IBaseFilter(base->pinInfo.pFilter);
    FIXME("(%p)->(%d %p)\n", This, iPosition, amt);
    return E_NOTIMPL;
}

static const BasePinFuncTable AVICompressorOutputBasePinVtbl = {
    NULL,
    BaseOutputPinImpl_AttemptConnection,
    AVICompressorOut_GetMediaTypeVersion,
    AVICompressorOut_GetMediaType
};

static HRESULT WINAPI AVICompressorOut_DecideBufferSize(BaseOutputPin *base, IMemAllocator *alloc, ALLOCATOR_PROPERTIES *ppropInputRequest)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI AVICompressorOut_DecideAllocator(BaseOutputPin *base,
        IMemInputPin *pPin, IMemAllocator **pAlloc)
{
    TRACE("(%p)->(%p %p)\n", base, pPin, pAlloc);
    return BaseOutputPinImpl_DecideAllocator(base, pPin, pAlloc);
}

static HRESULT WINAPI AVICompressorOut_BreakConnect(BaseOutputPin *base)
{
    FIXME("(%p)\n", base);
    return E_NOTIMPL;
}

static const BaseOutputPinFuncTable AVICompressorBaseOutputPinVtbl = {
    AVICompressorOut_DecideBufferSize,
    AVICompressorOut_DecideAllocator,
    AVICompressorOut_BreakConnect
};

IUnknown* WINAPI QCAP_createAVICompressor(IUnknown *outer, HRESULT *phr)
{
    PIN_INFO in_pin_info  = {NULL, PINDIR_INPUT,  {'I','n','p','u','t',0}};
    PIN_INFO out_pin_info = {NULL, PINDIR_OUTPUT, {'O','u','t','p','u','t',0}};
    AVICompressor *compressor;
    HRESULT hres;

    TRACE("\n");

    compressor = heap_alloc_zero(sizeof(*compressor));
    if(!compressor) {
        *phr = E_NOINTERFACE;
        return NULL;
    }

    BaseFilter_Init(&compressor->filter, &AVICompressorVtbl, &CLSID_AVICo,
            (DWORD_PTR)(__FILE__ ": AVICompressor.csFilter"), &filter_func_table);

    compressor->IPersistPropertyBag_iface.lpVtbl = &PersistPropertyBagVtbl;

    in_pin_info.pFilter = &compressor->filter.IBaseFilter_iface;
    hres = BaseInputPin_Construct(&AVICompressorInputPinVtbl, sizeof(BaseInputPin), &in_pin_info,
            &AVICompressorInputBasePinVtbl, &AVICompressorBaseInputPinVtbl,
            &compressor->filter.csFilter, NULL, (IPin**)&compressor->in);
    if(FAILED(hres)) {
        IBaseFilter_Release(&compressor->filter.IBaseFilter_iface);
        *phr = hres;
        return NULL;
    }

    out_pin_info.pFilter = &compressor->filter.IBaseFilter_iface;
    hres = BaseOutputPin_Construct(&AVICompressorOutputPinVtbl, sizeof(BaseOutputPin), &out_pin_info,
            &AVICompressorOutputBasePinVtbl, &AVICompressorBaseOutputPinVtbl,
            &compressor->filter.csFilter, (IPin**)&compressor->out);
    if(FAILED(hres)) {
        IBaseFilter_Release(&compressor->filter.IBaseFilter_iface);
        *phr = hres;
        return NULL;
    }

    *phr = S_OK;
    return (IUnknown*)&compressor->filter.IBaseFilter_iface;
}
