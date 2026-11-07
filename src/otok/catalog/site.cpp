// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/catalog/catalog.hpp"

namespace otok {

namespace {

// embedded_files: written by cmake/embed.cmake from src/otok/catalog/site/
#include "otok_catalog_site.inc"

}

std::span<embedded_file const> catalog_site_files()
{
    return embedded_files;
}

}
