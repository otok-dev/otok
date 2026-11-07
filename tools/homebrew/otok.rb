# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# The Homebrew formula of otok (docs/PLAN.md decision 50), written for homebrew-core: it builds the release's source
# archive (tools/release/source-archive.sh) against Homebrew's packages. Lua is part of otok's sources and compiled
# as C++ (third_party/lua/README.md); Homebrew's lua is the C build. The url and sha256 are the release's.
class Otok < Formula
  desc "Build native libraries from source and hand them to your build"
  homepage "https://github.com/otok-dev/otok"
  url "https://github.com/otok-dev/otok/releases/download/v0.2.0/otok-0.2.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  # with the Otok Output Exception (LICENSE-EXCEPTION), which is not in the SPDX list that Homebrew's license DSL takes
  license "GPL-3.0-only"

  depends_on "cmake" => :build
  depends_on "inja" => :build
  depends_on "nlohmann-json" => :build
  depends_on "libarchive"
  depends_on "tomlplusplus"

  uses_from_macos "curl"

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

    # a project without dependencies resolves and locks offline
    (testpath/"otok.toml").write <<~TOML
      [otok]
      format = 1

      [native]
      platforms = ["native"]
    TOML
    system bin/"otok", "install", "--offline"
    assert_path_exists testpath/"otok.lock"
  end
end
