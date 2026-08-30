"""Module configuration for the Luisa Compute companion (Vulkan host-import)."""


def can_build(env, platform):
    # Opt-in: only build when luisa_compute=yes. Off by default so it never
    # affects normal engine builds.
    if not env.get("luisa_compute", False):
        return False
    # The bridge imports Godot's Vulkan VkInstance/VkDevice/queues into a
    # compute-only Luisa Device. It therefore requires the Vulkan driver and
    # Volk (so the loader identity matches the one Luisa's backend uses).
    if platform not in ("windows", "linuxbsd", "macos"):
        return False
    if not env.get("vulkan", False):
        return False
    if not env.get("use_volk", False):
        return False
    return True


def configure(env):
    pass


def get_doc_classes():
    return ["LuisaCompute"]


def get_doc_path():
    return "doc_classes"
