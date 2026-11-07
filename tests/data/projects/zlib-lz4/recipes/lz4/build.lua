-- The CMake project is in build/cmake; the static library is liblz4.a, or lz4.lib with MSVC.
function build(slice, options, deps)
    otok.cmake{
        source = slice.source .. "/build/cmake",
        defines = { BUILD_SHARED_LIBS = false, BUILD_STATIC_LIBS = true, LZ4_BUILD_CLI = false },
    }
    local libs = otok.glob("lib/*.a", slice.prefix)
    for _, lib in ipairs(otok.glob("lib/*.lib", slice.prefix)) do table.insert(libs, lib) end
    assert(#libs == 1, "expected one library, found " .. #libs)
    return { libraries = { lz4 = libs[1] }, headers = slice.prefix .. "/include" }
end
