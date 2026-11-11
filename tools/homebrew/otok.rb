# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# The Homebrew formula of otok (docs/PLAN.md decisions 50 and 99), written for homebrew-core: it builds the release's
# source archive (tools/release/source-archive.sh) against Homebrew's packages. Lua is part of otok's sources and
# compiled as C++ (third_party/lua/README.md); Homebrew's lua is the C build. The url and sha256 are a release's:
# placeholders in otok's tools/homebrew/otok.rb, set by tools/release/packages.sh in the formula of otok-dev/homebrew-tap.
class Otok < Formula
  desc "Build native libraries from source and hand them to your build"
  homepage "https://github.com/otok-dev/otok"
  url "https://github.com/otok-dev/otok/releases/download/v0.0.0/otok-0.0.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  # with the Otok Output Exception (LICENSE-EXCEPTION), which is not in the SPDX list that Homebrew's license DSL takes
  license "GPL-3.0-only"
  head "https://github.com/otok-dev/otok.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "inja" => :build
  depends_on "nlohmann-json" => :build
  depends_on "libarchive"
  depends_on "tomlplusplus"

  uses_from_macos "curl"

  # C++23: GCC 14 or later (Homebrew's own GCC on Linux, decision 104)
  fails_with :gcc do
    version "13"
    cause "otok is written in C++23"
  end

  def install
    args = %W[
      -DOTOK_BUILD_TESTS=OFF
      -DOTOK_REVISION=#{version}
    ]
    system "cmake", "-S", ".", "-B", "build", *args, *std_cmake_args
    system "cmake", "--build", "build"
    bin.install "build/otok"
  end

  test do
    assert_match "otok #{version}", shell_output("#{bin}/otok --version")

    # a project without dependencies resolves and locks offline, for the target of the machine
    target = if OS.mac?
      "[apple]\nplatforms = [\"macosx\"]\ndeployment = { macos = \"14.0\" }\n"
    else
      "[linux]\nbuild-on = [\"linux/gcc\"]\n"
    end
    (testpath/"otok.toml").write "[otok]\nformat = 1\n\n#{target}"
    system bin/"otok", "install", "--offline"
    assert_path_exists testpath/"otok.lock"
  end
end
