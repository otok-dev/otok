function build(slice, options, deps)
    otok.cmake{}
    local libs = otok.glob("lib/*compiled*", slice.prefix)
    assert(#libs == 1, "expected one library, found " .. #libs)
    return { libraries = { compiled = libs[1] }, headers = slice.prefix .. "/include" }
end
