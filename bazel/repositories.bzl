# The build rules below for external dependencies build rules are maintained on a best effort basis.
# The rules are provided for developer convenience. For production builds, we recommend building the
# libraries according to their canonical build systems and expressing the dependencies in a manner
# similar to ci/WORKSPACE.

def googletest_repositories():
    BUILD = """
cc_library(
    name = "googletest",
    srcs = [
        "googlemock/src/gmock-all.cc",
        "googletest/src/gtest-all.cc",
    ],
    hdrs = glob([
        "googlemock/include/**/*.h",
        "googlemock/src/*.cc",
        "googletest/include/**/*.h",
        "googletest/src/*.cc",
        "googletest/src/*.h",
    ]),
    includes = [
        "googlemock",
        "googlemock/include",
        "googletest",
        "googletest/include",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "googletest_main",
    srcs = ["googlemock/src/gmock_main.cc"],
    visibility = ["//visibility:public"],
    deps = [":googletest"],
)
"""
    native.new_git_repository(
        name = "googletest",
        build_file_content = BUILD,
        # v1.8.0 release
        commit = "ec44c6c1675c25b9827aacd08c02433cccde7780",
        remote = "https://github.com/google/googletest.git",
    )

def spdlog_repositories():
    BUILD = """
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "spdlog",
    hdrs = glob([
        "include/**/*.cc",
        "include/**/*.h",
    ]),
    strip_include_prefix = "include",
)
"""

    native.new_git_repository(
        name = "spdlog_git",
        build_file_content = BUILD,
        # v0.11.0 release
        commit = "1f1f6a5f3b424203a429e9cb78e6548037adefa8",
        remote = "https://github.com/gabime/spdlog.git",
    )

def envoy_dependencies():
    googletest_repositories()
    spdlog_repositories()
