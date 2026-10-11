"""Generate COM forwarding methods from the installed Windows SDK declaration.

This reads headers and writes C++ source only. It does not invoke a compiler.
"""
import pathlib
import re
import sys

sdk = pathlib.Path(sys.argv[1])
root = pathlib.Path(__file__).resolve().parents[1]
text = sdk.read_text(encoding="utf-8", errors="replace")
factory_custom = {"QueryInterface", "AddRef", "Release", "CreateDevice"}
device_custom = {"QueryInterface", "AddRef", "Release", "Reset", "Present", "BeginScene", "Clear",
                 "SetTransform", "CreateVertexShader", "DrawPrimitive", "DrawIndexedPrimitive",
                 "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "StretchRect", "ColorFill",
                 "UpdateTexture", "UpdateSurface", "BeginStateBlock", "EndStateBlock", "CreateQuery",
                 "GetDirect3D", "SetRenderTarget", "TestCooperativeLevel"}

for interface, custom, output in [("IDirect3D9", factory_custom, "factory_forward.inc"),
                                   ("IDirect3DDevice9", device_custom, "device_forward.inc")]:
    start = text.index(f"DECLARE_INTERFACE_({interface}, IUnknown)")
    block = text[start:text.index("};", start)]
    methods = re.findall(r"STDMETHOD(?:_\(([^,]+),\s*(\w+)\)|\((\w+)\))\(THIS(?:_|\s*)\s*(.*?)\)\s*PURE;", block, re.S)
    lines = ["// Generated from the Microsoft Windows SDK COM declarations; regenerate with tools/generate_forwarders.py."]
    names = set()
    for ret, alternate, normal, params in methods:
        name = alternate or normal
        names.add(name)
        if name in custom:
            continue
        args = []
        declarations = []
        for parameter_index, param in enumerate(params.split(",")):
            if not param.strip():
                continue
            match = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", param)
            if not match or not re.search(r"[\s*&]", param.strip()):
                argument = f"argument{parameter_index}"
                param = param.strip() + " " + argument
            else:
                argument = match[1]
            args.append(argument)
            declarations.append(param.strip())
        guard = "std::lock_guard<std::recursive_mutex> lock(deviceMutex_); " if interface == "IDirect3DDevice9" else ""
        lines.append(f"{ret or 'HRESULT'} STDMETHODCALLTYPE {name}({', '.join(declarations)}) override {{ {guard}return real_->{name}({', '.join(args)}); }}")
    missing = custom - names
    if missing:
        raise ValueError(f"Missing custom methods: {missing}")
    target = root / "src" / "hook" / output
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{interface}: {len(methods)} methods; {len(lines)-1} forwarders -> {target}")
