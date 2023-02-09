/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_ADDON && !RESHADE_ADDON_LITE

#include "hook_manager.hpp"

template <size_t vtable_index, typename R, typename T, typename... Args>
static inline R call_vtable(T *object, Args... args)
{
	const auto vtable_entry = vtable_from_instance(object) + vtable_index;
	const auto func = reshade::hooks::is_hooked(vtable_entry) ?
		reshade::hooks::call<R(STDMETHODCALLTYPE *)(T *, Args...)>(nullptr, vtable_entry) :
		reinterpret_cast<R(STDMETHODCALLTYPE *)(T *, Args...)>(*vtable_entry);
	return func(object, std::forward<Args>(args)...);
}

#undef IDirect3DSurface9_LockRect
#define IDirect3DSurface9_LockRect call_vtable<13, HRESULT, IDirect3DSurface9, D3DLOCKED_RECT *, const RECT *, DWORD>
#undef IDirect3DSurface9_UnlockRect
#define IDirect3DSurface9_UnlockRect call_vtable<14, HRESULT, IDirect3DSurface9>

#undef IDirect3DVolume9_LockBox
#define IDirect3DVolume9_LockBox call_vtable<9, HRESULT, IDirect3DVolume9, D3DLOCKED_BOX *, const D3DBOX *, DWORD>
#undef IDirect3DVolume9_UnlockBox
#define IDirect3DVolume9_UnlockBox call_vtable<10, HRESULT, IDirect3DVolume9>

#undef IDirect3DTexture9_LockRect
#define IDirect3DTexture9_LockRect call_vtable<19, HRESULT, IDirect3DTexture9, UINT, D3DLOCKED_RECT *, const RECT *, DWORD>
#undef IDirect3DTexture9_UnlockRect
#define IDirect3DTexture9_UnlockRect call_vtable<20, HRESULT, IDirect3DTexture9, UINT>

#undef IDirect3DVolumeTexture9_LockBox
#define IDirect3DVolumeTexture9_LockBox call_vtable<19, HRESULT, IDirect3DVolumeTexture9, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD>
#undef IDirect3DVolumeTexture9_UnlockBox
#define IDirect3DVolumeTexture9_UnlockBox call_vtable<20, HRESULT, IDirect3DVolumeTexture9, UINT>

#undef IDirect3DCubeTexture9_LockRect
#define IDirect3DCubeTexture9_LockRect call_vtable<19, HRESULT, IDirect3DCubeTexture9, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD>
#undef IDirect3DCubeTexture9_UnlockRect
#define IDirect3DCubeTexture9_UnlockRect call_vtable<20, HRESULT, IDirect3DCubeTexture9, D3DCUBEMAP_FACES, UINT>

#undef IDirect3DVertexBuffer9_Lock
#define IDirect3DVertexBuffer9_Lock call_vtable<11, HRESULT, IDirect3DVertexBuffer9, UINT, UINT, void **, DWORD>
#undef IDirect3DVertexBuffer9_Unlock
#define IDirect3DVertexBuffer9_Unlock call_vtable<12, HRESULT, IDirect3DVertexBuffer9>

#undef IDirect3DVertexBuffer9_SetPrivateData
#define IDirect3DVertexBuffer9_SetPrivateData call_vtable<4, HRESULT, IDirect3DVertexBuffer9, REFGUID, CONST void *, DWORD, DWORD>

#undef IDirect3DIndexBuffer9_Lock
#define IDirect3DIndexBuffer9_Lock call_vtable<11, HRESULT, IDirect3DIndexBuffer9, UINT, UINT, void **, DWORD>
#undef IDirect3DIndexBuffer9_Unlock
#define IDirect3DIndexBuffer9_Unlock call_vtable<12, HRESULT, IDirect3DIndexBuffer9>

#undef IDirect3DResource9_SetPrivateData
#define IDirect3DResource9_SetPrivateData call_vtable<4, HRESULT, IDirect3DResource9, REFGUID, CONST void *, DWORD, DWORD>

#endif
