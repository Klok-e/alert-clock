# Sanitizers for the PC build, applied to this project's sources (and the header-only
# libraries they include) but not to third-party library sources such as LovyanGFX,
# whose PC backend trips UBSan's alignment check in its PNG export.
Import("env")  # noqa: F821  (provided by SCons)

SANITIZE = ("-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer")

env.Append(  # noqa: F821
    CCFLAGS=list(SANITIZE),
    LINKFLAGS=["-fsanitize=address,undefined"],
)


def third_party_unsanitized(env, node):
    if "libdeps" not in node.srcnode().get_abspath():
        return node
    return env.Object(node, CCFLAGS=[flag for flag in env["CCFLAGS"] if flag not in SANITIZE])


env.AddBuildMiddleware(third_party_unsanitized)  # noqa: F821
