"""Generates runtime/d3d9_bridge_gen.cpp: guest-callable shims for every
Direct3D 9 COM method, forwarding to a host d3d9 implementation (DXVK).

Argument kinds:
  u   32-bit value passed through          i   signed 32-bit
  f   float (bits reinterpreted)            p   guest pointer -> host pointer (or NULL)
  o:T guest interface pointer -> host T*    O:T guest T** out -> wrapped host result
  h   HANDLE* shared handle (ignored, NULL) w   HWND (mapped to the host window)
Methods marked X are hand-written in d3d9_bridge.cpp.
"""
import os
import sys

UNK = [("QueryInterface", "X"), ("AddRef", "X"), ("Release", "X")]
RES = UNK + [
    ("GetDevice", "O:IDirect3DDevice9"), ("SetPrivateData", "p p u u"), ("GetPrivateData", "p p p"),
    ("FreePrivateData", "p"), ("SetPriority", "u"), ("GetPriority", ""), ("PreLoad", "V"), ("GetType", ""),
]
BASETEX = RES + [
    ("SetLOD", "u"), ("GetLOD", ""), ("GetLevelCount", ""), ("SetAutoGenFilterType", "u"),
    ("GetAutoGenFilterType", ""), ("GenerateMipSubLevels", "V"),
]

IFACES = {
    "IDirect3D9": UNK + [
        ("RegisterSoftwareDevice", "p"), ("GetAdapterCount", ""), ("GetAdapterIdentifier", "X"),
        ("GetAdapterModeCount", "X"), ("EnumAdapterModes", "X"), ("GetAdapterDisplayMode", "X"),
        ("CheckDeviceType", "u u u u u"), ("CheckDeviceFormat", "X"),
        ("CheckDeviceMultiSampleType", "u u u u u p"), ("CheckDepthStencilMatch", "u u u u u"),
        ("CheckDeviceFormatConversion", "u u u u"), ("GetDeviceCaps", "u u p"), ("GetAdapterMonitor", "X"),
        ("CreateDevice", "X"),
    ],
    "IDirect3DDevice9": UNK + [
        ("TestCooperativeLevel", ""), ("GetAvailableTextureMem", ""), ("EvictManagedResources", ""),
        ("GetDirect3D", "O:IDirect3D9"), ("GetDeviceCaps", "p"), ("GetDisplayMode", "u p"),
        ("GetCreationParameters", "X"), ("SetCursorProperties", "u u o:IDirect3DSurface9"),
        ("SetCursorPosition", "Vi i u"), ("ShowCursor", "u"), ("CreateAdditionalSwapChain", "X"),
        ("GetSwapChain", "u O:IDirect3DSwapChain9"), ("GetNumberOfSwapChains", ""), ("Reset", "X"),
        ("Present", "X"), ("GetBackBuffer", "u u u O:IDirect3DSurface9"), ("GetRasterStatus", "u p"),
        ("SetDialogBoxMode", "u"), ("SetGammaRamp", "Vu u p"), ("GetGammaRamp", "Vu p"),
        ("CreateTexture", "X"),
        ("CreateVolumeTexture", "u u u u u u u O:IDirect3DVolumeTexture9 h"),
        ("CreateCubeTexture", "X"),
        ("CreateVertexBuffer", "u u u u O:IDirect3DVertexBuffer9 h"),
        ("CreateIndexBuffer", "u u u u O:IDirect3DIndexBuffer9 h"),
        ("CreateRenderTarget", "u u u u u u O:IDirect3DSurface9 h"),
        ("CreateDepthStencilSurface", "u u u u u u O:IDirect3DSurface9 h"),
        ("UpdateSurface", "X"),
        ("UpdateTexture", "X"),
        ("GetRenderTargetData", "o:IDirect3DSurface9 o:IDirect3DSurface9"),
        ("GetFrontBufferData", "u o:IDirect3DSurface9"),
        ("StretchRect", "o:IDirect3DSurface9 p o:IDirect3DSurface9 p u"),
        ("ColorFill", "o:IDirect3DSurface9 p u"),
        ("CreateOffscreenPlainSurface", "u u u u O:IDirect3DSurface9 h"),
        ("SetRenderTarget", "u o:IDirect3DSurface9"), ("GetRenderTarget", "u O:IDirect3DSurface9"),
        ("SetDepthStencilSurface", "o:IDirect3DSurface9"), ("GetDepthStencilSurface", "O:IDirect3DSurface9"),
        ("BeginScene", ""), ("EndScene", ""), ("Clear", "u p u u f u"),
        ("SetTransform", "u p"), ("GetTransform", "u p"), ("MultiplyTransform", "u p"),
        ("SetViewport", "p"), ("GetViewport", "p"), ("SetMaterial", "p"), ("GetMaterial", "p"),
        ("SetLight", "u p"), ("GetLight", "u p"), ("LightEnable", "u u"), ("GetLightEnable", "u p"),
        ("SetClipPlane", "u p"), ("GetClipPlane", "u p"), ("SetRenderState", "u u"), ("GetRenderState", "u p"),
        ("CreateStateBlock", "u O:IDirect3DStateBlock9"), ("BeginStateBlock", ""),
        ("EndStateBlock", "O:IDirect3DStateBlock9"), ("SetClipStatus", "p"), ("GetClipStatus", "p"),
        ("GetTexture", "u O:IDirect3DBaseTexture9"), ("SetTexture", "u o:IDirect3DBaseTexture9"),
        ("GetTextureStageState", "u u p"), ("SetTextureStageState", "u u u"),
        ("GetSamplerState", "u u p"), ("SetSamplerState", "u u u"), ("ValidateDevice", "p"),
        ("SetPaletteEntries", "u p"), ("GetPaletteEntries", "u p"), ("SetCurrentTexturePalette", "u"),
        ("GetCurrentTexturePalette", "p"), ("SetScissorRect", "p"), ("GetScissorRect", "p"),
        ("SetSoftwareVertexProcessing", "u"), ("GetSoftwareVertexProcessing", ""), ("SetNPatchMode", "f"),
        ("GetNPatchMode", "X"), ("DrawPrimitive", "u u u"), ("DrawIndexedPrimitive", "u i u u u u"),
        ("DrawPrimitiveUP", "u u p u"), ("DrawIndexedPrimitiveUP", "u u u u p u p u"),
        ("ProcessVertices", "u u u o:IDirect3DVertexBuffer9 o:IDirect3DVertexDeclaration9 u"),
        ("CreateVertexDeclaration", "p O:IDirect3DVertexDeclaration9"),
        ("SetVertexDeclaration", "o:IDirect3DVertexDeclaration9"),
        ("GetVertexDeclaration", "O:IDirect3DVertexDeclaration9"), ("SetFVF", "u"), ("GetFVF", "p"),
        ("CreateVertexShader", "p O:IDirect3DVertexShader9"), ("SetVertexShader", "o:IDirect3DVertexShader9"),
        ("GetVertexShader", "O:IDirect3DVertexShader9"),
        ("SetVertexShaderConstantF", "u p u"), ("GetVertexShaderConstantF", "u p u"),
        ("SetVertexShaderConstantI", "u p u"), ("GetVertexShaderConstantI", "u p u"),
        ("SetVertexShaderConstantB", "u p u"), ("GetVertexShaderConstantB", "u p u"),
        ("SetStreamSource", "u o:IDirect3DVertexBuffer9 u u"), ("GetStreamSource", "u O:IDirect3DVertexBuffer9 p p"),
        ("SetStreamSourceFreq", "u u"), ("GetStreamSourceFreq", "u p"),
        ("SetIndices", "o:IDirect3DIndexBuffer9"), ("GetIndices", "O:IDirect3DIndexBuffer9"),
        ("CreatePixelShader", "p O:IDirect3DPixelShader9"), ("SetPixelShader", "o:IDirect3DPixelShader9"),
        ("GetPixelShader", "O:IDirect3DPixelShader9"),
        ("SetPixelShaderConstantF", "u p u"), ("GetPixelShaderConstantF", "u p u"),
        ("SetPixelShaderConstantI", "u p u"), ("GetPixelShaderConstantI", "u p u"),
        ("SetPixelShaderConstantB", "u p u"), ("GetPixelShaderConstantB", "u p u"),
        ("DrawRectPatch", "u p p"), ("DrawTriPatch", "u p p"), ("DeletePatch", "u"),
        ("CreateQuery", "u O:IDirect3DQuery9"),
    ],
    "IDirect3DSwapChain9": UNK + [
        ("Present", "X"), ("GetFrontBufferData", "o:IDirect3DSurface9"),
        ("GetBackBuffer", "u u O:IDirect3DSurface9"), ("GetRasterStatus", "p"), ("GetDisplayMode", "p"),
        ("GetDevice", "O:IDirect3DDevice9"), ("GetPresentParameters", "X"),
    ],
    "IDirect3DTexture9": BASETEX + [
        ("GetLevelDesc", "X"), ("GetSurfaceLevel", "X"), ("LockRect", "X"),
        ("UnlockRect", "X"), ("AddDirtyRect", "p"),
    ],
    "IDirect3DCubeTexture9": BASETEX + [
        ("GetLevelDesc", "X"), ("GetCubeMapSurface", "X"), ("LockRect", "X"),
        ("UnlockRect", "X"), ("AddDirtyRect", "u p"),
    ],
    "IDirect3DVolumeTexture9": BASETEX + [
        ("GetLevelDesc", "u p"), ("GetVolumeLevel", "u O:IDirect3DVolume9"), ("LockBox", "X"),
        ("UnlockBox", "X"), ("AddDirtyBox", "p"),
    ],
    "IDirect3DVolume9": UNK + [
        ("GetDevice", "O:IDirect3DDevice9"), ("SetPrivateData", "p p u u"), ("GetPrivateData", "p p p"),
        ("FreePrivateData", "p"), ("GetContainer", "X"), ("GetDesc", "p"), ("LockBox", "X"), ("UnlockBox", "X"),
    ],
    "IDirect3DSurface9": RES + [
        ("GetContainer", "X"), ("GetDesc", "X"), ("LockRect", "X"), ("UnlockRect", "X"), ("GetDC", "X"),
        ("ReleaseDC", "X"),
    ],
    "IDirect3DVertexBuffer9": RES + [("Lock", "X"), ("Unlock", "X"), ("GetDesc", "p")],
    "IDirect3DIndexBuffer9": RES + [("Lock", "X"), ("Unlock", "X"), ("GetDesc", "p")],
    "IDirect3DVertexDeclaration9": UNK + [("GetDevice", "O:IDirect3DDevice9"), ("GetDeclaration", "p p")],
    "IDirect3DVertexShader9": UNK + [("GetDevice", "O:IDirect3DDevice9"), ("GetFunction", "p p")],
    "IDirect3DPixelShader9": UNK + [("GetDevice", "O:IDirect3DDevice9"), ("GetFunction", "p p")],
    "IDirect3DStateBlock9": UNK + [("GetDevice", "O:IDirect3DDevice9"), ("Capture", ""), ("Apply", "")],
    "IDirect3DQuery9": UNK + [
        ("GetDevice", "O:IDirect3DDevice9"), ("GetType", ""), ("GetDataSize", ""), ("Issue", "u"),
        ("GetData", "p u u"),
    ],
}

# Methods whose return type is not HRESULT/DWORD-like and need a cast.
VOID_RET = {"PreLoad", "GenerateMipSubLevels"}


def gen():
    o = []
    o.append("// Generated by tools/gen_d3d9_bridge.py - do not edit.")
    o.append('#include "d3d9_bridge.h"')
    o.append("")
    hand = []
    for iface, methods in IFACES.items():
        for idx, (name, spec) in enumerate(methods):
            fn = "B_%s_%s" % (iface, name)
            if spec == "X" and name in ("QueryInterface", "AddRef", "Release"):
                o.append("static void %s(CPU *c) { com_%s(c); }" % (fn, name))
                continue
            if spec == "X":
                hand.append(fn)
                continue
            void_ret = spec.startswith("V")
            spec = spec[1:] if void_ret else spec
            args = spec.split() if spec else []
            o.append("static void %s(CPU *c) {" % fn)
            o.append("    %s *self = (%s *)com_host(ARG(0));" % (iface, iface))
            o.append("    if (g_d3d_trace) d3d_trace(c, \"%s::%s\", %d);" % (iface, name, len(args)))
            if name.startswith("Draw") and not name.endswith("Patch"):
                o.append("    if (g_bisect && d3d_skip_draw()) { RET(0, %d); return; }" % (len(args) + 1))
            call = []
            post = []
            for k, a in enumerate(args):
                g = "ARG(%d)" % (k + 1)
                if a == "u":
                    call.append(g)
                elif a == "i":
                    call.append("(INT)%s" % g)
                elif a == "f":
                    call.append("ARGF(%d)" % (k + 1))
                elif a == "p":
                    call.append("GPTR(%s)" % g)
                elif a == "h":
                    call.append("nullptr")
                elif a.startswith("o:"):
                    call.append("(%s *)com_host(%s)" % (a[2:], g))
                elif a.startswith("O:"):
                    t = a[2:]
                    o.append("    %s *out%d = nullptr;" % (t, k))
                    call.append("&out%d" % k)
                    post.append("    if (%s) M32(%s) = d3d9_wrap(out%d, IFACE_%s);" % (g, g, k, t))
                else:
                    raise ValueError(a)
            # cast enums: the host signatures take enum types; use a variadic-safe template call
            if void_ret:
                o.append("    bridge_call(self, &%s::%s%s%s);" % (iface, name, ", " if call else "", ", ".join(call)))
                o.extend(post)
                o.append("    RET(0, %d);" % (len(args) + 1))
                o.append("}")
                o.append("")
                continue
            o.append("    auto r = bridge_call(self, &%s::%s%s%s);" % (iface, name, ", " if call else "", ", ".join(call)))
            o.extend(post)
            o.append("    RET((uint32_t)r, %d);" % (len(args) + 1))
            o.append("}")
            o.append("")
    # hand-written declarations
    for fn in hand:
        o.append("void %s(CPU *c);" % fn)
    o.append("")
    # vtables
    for iface, methods in IFACES.items():
        o.append("static const ShimFn vt_%s[] = {" % iface)
        for name, spec in methods:
            o.append("    B_%s_%s," % (iface, name))
        o.append("};")
    o.append("")
    o.append("void d3d9_register_vtables(void) {")
    for iface, methods in IFACES.items():
        o.append('    com_register_iface(IFACE_%s, "%s", vt_%s, %d);' % (iface, iface, iface, len(methods)))
    o.append("}")
    return "\n".join(o) + "\n"


def gen_header():
    o = ["// Generated by tools/gen_d3d9_bridge.py - do not edit.", "#pragma once", "enum IfaceId {"]
    for iface in IFACES:
        o.append("    IFACE_%s," % iface)
    o.append("    IFACE_IDirect3DBaseTexture9,")
    o.append("    IFACE_COUNT_D3D9")
    o.append("};")
    return "\n".join(o) + "\n"


out = sys.argv[1]
open(os.path.join(out, "d3d9_bridge_gen.cpp"), "w").write(gen())
open(os.path.join(out, "d3d9_ifaces.h"), "w").write(gen_header())
print("ok", sum(len(m) for m in IFACES.values()), "methods")
