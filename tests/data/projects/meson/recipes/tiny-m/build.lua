function build(slice, options, deps)
    assert(otok.read(otok.meson_cross_file()):find("[binaries]", 1, true))
    otok.meson{ options = { value = options.value } }
    -- a DLL with its import library on Windows (meson installs the DLL into bin/)
    local dll = otok.glob("bin/*tiny_m*.dll", slice.prefix)[1]
    local libs = otok.glob("lib/*tiny_m*", slice.prefix)
    assert(#libs == 1, "expected one library, found " .. #libs)
    local lib = dll and { link = libs[1], runtime = dll } or libs[1]
    return { libraries = { tiny_m = lib }, headers = slice.prefix .. "/include" }
end
