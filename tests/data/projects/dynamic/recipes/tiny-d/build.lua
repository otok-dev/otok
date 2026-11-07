-- CMake builds it as slice.linkage asks (BUILD_SHARED_LIBS); what it installs depends on the host.
function build(slice, options, deps)
    otok.cmake{}
    local p = slice.prefix
    local dll = otok.glob("bin/*tiny_d*.dll", p)
    if #dll == 1 then
        -- Windows: the import library and the DLL
        return { libraries = { tiny_d = { link = otok.glob("lib/*tiny_d*.lib", p)[1], runtime = dll[1] } }, headers = p .. "/include" }
    end
    -- a shared object or dynamic library under its link name (the links come along), or the static archive
    local lib = otok.glob("lib/libtiny_d.so", p)[1] or otok.glob("lib/libtiny_d.dylib", p)[1] or otok.glob("lib/*tiny_d*", p)[1]
    return { libraries = { tiny_d = lib }, headers = p .. "/include" }
end
