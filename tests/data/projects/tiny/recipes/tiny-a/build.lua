function build(slice, options, deps)
    otok.cmake{ defines = { TINY_A_VALUE = options.value } }
    local libs = otok.glob("lib/*tiny_a*", slice.prefix)  -- libtiny_a.a, or tiny_a.lib with MSVC
    assert(#libs == 1, "expected one library, found " .. #libs)
    return { libraries = { tiny_a = libs[1] }, headers = slice.prefix .. "/include" }
end
