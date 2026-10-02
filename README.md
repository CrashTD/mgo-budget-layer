# MGO on Linux: Vulkan layer for the CSX memory check

> Not a coder – Claude wrote most of it, I only tested it on my rig. If it sets your PC on fire, blame the robot. 😉

**Problem:** Community Shaders (CSX) checks the DXGI memory budget before it switches to its scaled render targets (Render Scale / FSR4). Under Proton that number comes from DXVK, which passes on the `VK_EXT_memory_budget` value unchanged, and it's too low for two reasons: on RADV the budget is the VRAM size minus what other processes hold (desktop, compositor, VR runtime), and the usage side also counts Proton's separate FSR4 device (vkd3d, ~1.5 GB) in the same process. On a 16 GB card CSX saw e.g. 14110 MiB used of a 14734 MiB budget (96 %), which it treats as critical. CSX then silently falls back to native resolution and FSR4 costs more than it saves.

**Workaround:** a tiny Vulkan layer that only rewrites `heapBudget` of the VRAM heap in `vkGetPhysicalDeviceMemoryProperties2`. Nothing else changes. DXVK still caps its own allocations, so set `dxvk.maxMemoryBudget` as well (13500 on a 16 GB card, see the Discord post).

## Build

Clone this repo. Needs `gcc` and the Vulkan headers (`vulkan-headers` on Arch/CachyOS and Fedora, `libvulkan-dev` on Debian/Ubuntu).

```
gcc -shared -fPIC -O2 -o libVkLayer_mgo_budget_fake.so vk_budget_fake.c
```

Put `libVkLayer_mgo_budget_fake.so` and `VkLayer_mgo_budget_fake.json` in one folder (the repo folder itself is fine).

## Use

Add to the launch options (Fluorine / Steam):

```
MGO_BUDGET_FAKE_MIB=24000 MGO_BUDGET_FAKE_OVERSIZE=1 VK_ADD_LAYER_PATH=/path/to/that/folder VK_LOADER_LAYERS_ENABLE=VK_LAYER_MGO_budget_fake
```

Optional: `MGO_BUDGET_FAKE_LOG=/tmp/budget.log` writes a line per heap on the first calls, so you can see it's active.

My full line:

```
MGO_BUDGET_FAKE_MIB=24000 MGO_BUDGET_FAKE_OVERSIZE=1 VK_ADD_LAYER_PATH=/path/to/that/folder VK_LOADER_LAYERS_ENABLE=VK_LAYER_MGO_budget_fake DXVK_CONFIG="dxvk.maxMemoryBudget=13500;d3d11.cachedDynamicResources=c" PROTON_FSR4_UPGRADE=1 %command%
```

## License

GPL-3.0, see `LICENSE`.
