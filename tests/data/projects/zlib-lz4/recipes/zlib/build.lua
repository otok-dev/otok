-- The static library is libz.a, or zs.lib with MSVC.
function build(slice, options, deps)
    otok.cmake{ defines = { ZLIB_BUILD_SHARED = false, ZLIB_BUILD_TESTING = false } }
    local libs = otok.glob("lib/*.a", slice.prefix)
    for _, lib in ipairs(otok.glob("lib/*.lib", slice.prefix)) do table.insert(libs, lib) end
    assert(#libs == 1, "expected one library, found " .. #libs)
    return { libraries = { z = libs[1] }, headers = slice.prefix .. "/include" }
end
