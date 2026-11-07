-- libone.so and libtwo.so, which needs it, linked with the compiler directly (build-lua-api.md 3.1).
function build(slice, options, deps)
    local t = slice.toolchain
    local lib = slice.prefix .. "/lib"
    otok.mkdir(lib)
    otok.run{ t.cc, "-shared", "-fPIC", "-Wl,-soname,libone.so", "-o", lib .. "/libone.so", "one.c" }
    local two = { t.cc, "-shared", "-fPIC", "-Wl,-soname,libtwo.so", "-o", lib .. "/libtwo.so", "two.c", "-L" .. lib, "-lone" }
    if options.origin then two[#two + 1] = "-Wl,-rpath," .. t.rpath end
    otok.run(two)
    otok.mkdir(slice.prefix .. "/include")
    otok.copy("pair.h", slice.prefix .. "/include/pair.h")
    return { libraries = { one = lib .. "/libone.so", two = lib .. "/libtwo.so" }, headers = slice.prefix .. "/include" }
end
