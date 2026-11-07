-- The example of docs/formats/build-lua-api.md section 1.
function build(slice, options, deps)
    local configure = {slice.toolchains.perl, "./Configure", slice.vars.openssl_target,
        "no-shared", "no-tests", "--prefix=" .. slice.prefix}
    if options["no-deprecated"] then table.insert(configure, "no-deprecated") end
    for _, flag in ipairs(options.features) do table.insert(configure, flag) end

    otok.run(configure)
    otok.make{"build_libs"}
    otok.make{"install_dev"}

    return {
        libraries = { ssl = slice.prefix .. "/lib/libssl.a", crypto = slice.prefix .. "/lib/libcrypto.a" },
        headers = slice.prefix .. "/include",
    }
end
