// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/lock/lock.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/resolver/resolver.hpp"

// From a resolution to `otok.lock` and back (docs/formats/otok-lock.md 2, 3, 6).
namespace otok {

// How an input of a recipe is locked: its archive or git pins.
locked_location location_of(input_location const& in);

// The lock of a resolution. Local source overrides (`source = { path }`) inside the project are
// hashed, reading them from `project_dir`, unless `current` locks the same path for the library and
// the library is not in `rehash`: then the locked hash is kept (docs/PLAN.md decision 13), and the
// directory is checked against it before it is built (verify_local_source).
result<lock_file> make_lock(resolution const& r, manifest const& m, std::filesystem::path const& project_dir,
    lock_file const* current = nullptr, std::span<std::string const> rehash = {});

// Hashes a local source inside the project and compares it with the lock: an error when the
// directory changed since the lock was written (otok-lock.md 4).
result<void> verify_local_source(locked_package const& p, std::filesystem::path const& project_dir, std::string const& label);

// What differs between the current lock and a fresh one, one line per change, for `--locked` and for
// the summary of `install`/`update`: "added openssl 3.5.4 (r1)", "openssl: 3.5.3 (r1) -> 3.5.4 (r1)".
// Empty: the lock is up to date.
std::vector<std::string> lock_changes(lock_file const& current, lock_file const& fresh);

// Where a fresh lock contradicts the current one instead of updating it (otok-lock.md 4): a recipe
// fetched at its locked commit with another tree hash, or an unchanged recipe tree with other inputs.
// One message per package; empty: nothing contradicts.
std::vector<std::string> lock_conflicts(lock_file const& current, lock_file const& fresh);

// Packages whose inputs are not reproducible ("local"), which `--locked` refuses (otok-lock.md 3).
std::vector<std::string> local_packages(lock_file const& lock);

}
