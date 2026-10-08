"""Module configuration for the Luisa Compute companion (Vulkan / D3D12 host-import)."""


def can_build(env, platform):
    # Opt-in: only build when luisa_compute=yes. Off by default so it never
    # affects normal engine builds.
    if not env.get("luisa_compute", False):
        return False
    if platform not in ("windows", "linuxbsd", "macos"):
        return False
    # The bridge imports Godot's rendering device handles into a compute-only
    # Luisa Device. Either backend path is enough on its own:
    #  - Vulkan: needs the Vulkan driver plus Volk, so the loader identity
    #    matches the one Luisa's vk backend uses (VulkanDeviceConfigExt).
    #  - Direct3D 12: windows-only, needs the D3D12 driver so we can hand
    #    Godot's ID3D12Device to Luisa's dx backend (DirectXDeviceConfigExt).
    use_vulkan_path = env.get("vulkan", False) and env.get("use_volk", False)
    use_d3d12_path = platform == "windows" and env.get("d3d12", False)
    return bool(use_vulkan_path or use_d3d12_path)


def configure(env):
    pass


def get_doc_classes():
    return ["LuisaCompute"]


def get_doc_path():
    return "doc_classes"
