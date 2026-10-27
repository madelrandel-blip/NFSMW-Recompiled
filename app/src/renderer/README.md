# Vulkan 1.1 renderer

A from-scratch renderer for the native graphics path, selected with `nfsmw_renderer = "vulkan11"` in `nfsmw.toml`.
The previous renderer (`nfsmw_nativo_destinos.cpp`, `nfsmw_nativo_dibujos.cpp`) stays the default until this one
matches it.

## What it replaces and what it keeps

The native graphics system (`nfsmw_nativo_sistema.cpp`) reads the game's PM4 command ring, keeps the register file
and identifies the shaders of every draw. It hands three kinds of work to a renderer through the
`nativo::DestinosNativos` interface: draws, copies (resolves and clears) and swaps. This directory is a new
implementation of that interface; the system above it does not change.

## Requirements

Vulkan 1.1 core only. No descriptor indexing, no buffer device address, no 64-bit integers, no dynamic rendering, no
extended dynamic state. Optional extensions are used when present and never required.

The shaders come from the direct SPIR-V translator with `SpirvResourceModel::Vulkan11`
(`shaders/XenosRecomp/spirv_recompiler.h`): SPIR-V 1.3, textures as combined image samplers at fixed bindings of
set 0, constants in three dynamic uniform buffers of set 1.

## Modules

| File | Responsibility |
|---|---|
| `vulkan_context` | The SDK device as the renderer sees it: functions, queue, memory types, required features. |
| `frame_ring` | Frames in flight: command buffers, fences, a linear upload buffer and deferred destruction per frame. |
| `guest_state` | Typed views of the guest registers (render targets, blending, depth, rasterizer, fetch constants). |
| `render_targets` | EDRAM-less render targets: one image per (base, format, pitch), clears and resolves to textures. |
| `textures` | Guest textures: fetch constant decoding, untiling, format conversion, upload, cache and eviction. |
| `pipelines` | Render passes, pipeline layout and pipelines keyed by the shader pair and the fixed-function state. |
| `draws` | One guest draw: indices, vertices, constants, descriptors and the draw command. |
| `output` | The swap: the resolved front buffer through the game's gamma ramp into the presenter. |
| `vulkan_renderer` | Ties the modules together behind `nativo::DestinosNativos`. |

Every module owns its Vulkan objects, and destroys them only once the GPU is done with them (`frame_ring`).
