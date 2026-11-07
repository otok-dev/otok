// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include "otok/version/version.hpp"

// Internal to the version module: shared by version.cpp and constraint.cpp.
namespace otok::detail {

// The exclusive upper bound N of `~> operand` (docs/formats/README.md 3.2): the last given part
// may grow, so the part before it (or the only part) is incremented and its suffix dropped.
result<version> compatible_upper_bound(version_scheme scheme, version const& operand);

}
