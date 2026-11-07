-- CMake builds it as slice.linkage asks (BUILD_SHARED_LIBS); what it installs depends on the host.
function build(slice, options, deps)
    otok.cmake{}
    local p = slice.prefix
    local dll = otok.glob("bin/*tiny_c*.dll", p)
    if #dll == 1 then
        -- Windows: the import library and the DLL
        return { libraries = { tiny_c = { link = otok.glob("lib/*tiny_c*.lib", p)[1], runtime = dll[1] } }, headers = p .. "/include" }
    end
    -- a shared object or dynamic library under its link name (the links come along), or the static archive
    local lib = otok.glob("lib/libtiny_c.so", p)[1] or otok.glob("lib/libtiny_c.dylib", p)[1] or otok.glob("lib/*tiny_c*", p)[1]
    return { libraries = { tiny_c = lib }, headers = p .. "/include" }
end
