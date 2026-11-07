function build(slice, options, deps)
    assert(otok.exists(deps["tiny-a"].include .. "/tiny_a.h"))
    otok.cmake{}
    local libs = otok.glob("lib/*tiny_b*", slice.prefix)
    assert(#libs == 1, "expected one library, found " .. #libs)
    return { libraries = { tiny_b = libs[1] }, headers = slice.prefix .. "/include" }
end
