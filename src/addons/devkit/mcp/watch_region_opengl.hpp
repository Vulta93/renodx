/*
 * Copyright (C) 2026 Vulta93
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <type_traits>

#include <include/reshade.hpp>

// devkit_watch_region on OpenGL: the GL entry points the watch calls itself, and the state it keeps per session.
//
// ReShade needs OpenGL 4.3 (its wglMakeCurrent hook refuses to initialise below), so compute shaders, image load/store
// and atomics are available whenever the Devkit runs. The watch copies the region into a grid atlas through ReShade's API
// while the game records, and reduces the atlas with one compute dispatch at the frame end
// (watch_region_reduce_gl.comp.glsl). Everything the watch does to the game's GL state is listed at the frame-end pass in
// watch_region.hpp.
//
// The entry points are loaded the way gpu_fence::OpenGLFence loads them (opengl32 exports plus wglGetProcAddress), without a
// GL header: this file is included by an add-on that does not link opengl32.

namespace renodx::addons::devkit::mcp::watch_region {

namespace gl_enum {

constexpr unsigned int NONE = 0u;
constexpr unsigned int TEXTURE = 0x1702u;
constexpr unsigned int RENDERBUFFER = 0x8D41u;
constexpr unsigned int READ_FRAMEBUFFER = 0x8CA8u;
constexpr unsigned int DRAW_FRAMEBUFFER = 0x8CA9u;
constexpr unsigned int DRAW_FRAMEBUFFER_BINDING = 0x8CA6u;
constexpr unsigned int READ_FRAMEBUFFER_BINDING = 0x8CAAu;
constexpr unsigned int READ_BUFFER = 0x0C02u;
constexpr unsigned int DRAW_BUFFER0 = 0x8825u;
constexpr unsigned int MAX_DRAW_BUFFERS = 0x8824u;
constexpr unsigned int ATTACHMENT_OBJECT_TYPE = 0x8CD0u;
constexpr unsigned int ATTACHMENT_OBJECT_NAME = 0x8CD1u;
constexpr unsigned int ATTACHMENT_TEXTURE_LEVEL = 0x8CD2u;
constexpr unsigned int ATTACHMENT_TEXTURE_LAYER = 0x8CD4u;
constexpr unsigned int ATTACHMENT_LAYERED = 0x8DA7u;
constexpr unsigned int BACK_LEFT = 0x0402u;
constexpr unsigned int BACK_RIGHT = 0x0403u;
constexpr unsigned int BACK = 0x0405u;
constexpr unsigned int LEFT = 0x0406u;
constexpr unsigned int RIGHT = 0x0407u;
constexpr unsigned int FRONT_AND_BACK = 0x0408u;
constexpr unsigned int CURRENT_PROGRAM = 0x8B8Du;
// Target part of ReShade's pipeline handle for a program the application created ((PROGRAM << 40) | name).
constexpr unsigned int PROGRAM = 0x82E2u;
constexpr unsigned int MAX_TEXTURE_SIZE = 0x0D33u;
constexpr unsigned int MAX_IMAGE_UNITS = 0x8F38u;
constexpr unsigned int MAX_COMPUTE_WORK_GROUP_COUNT = 0x91BEu;
constexpr unsigned int IMAGE_BINDING_NAME = 0x8F3Au;
constexpr unsigned int IMAGE_BINDING_LEVEL = 0x8F3Bu;
constexpr unsigned int IMAGE_BINDING_LAYERED = 0x8F3Cu;
constexpr unsigned int IMAGE_BINDING_LAYER = 0x8F3Du;
constexpr unsigned int IMAGE_BINDING_ACCESS = 0x8F3Eu;
constexpr unsigned int IMAGE_BINDING_FORMAT = 0x906Eu;
constexpr unsigned int READ_ONLY = 0x88B8u;
constexpr unsigned int NUM_EXTENSIONS = 0x821Du;
constexpr unsigned int EXTENSIONS = 0x1F03u;
constexpr unsigned int ALPHA_TEST = 0x0BC0u;
constexpr unsigned int ALL_BARRIER_BITS = 0xFFFFFFFFu;
constexpr unsigned int SYNC_GPU_COMMANDS_COMPLETE = 0x9117u;
constexpr unsigned int SYNC_FLUSH_COMMANDS_BIT = 0x00000001u;
constexpr unsigned int ALREADY_SIGNALED = 0x911Au;
constexpr unsigned int CONDITION_SATISFIED = 0x911Cu;
constexpr unsigned int SYNC_WAIT_FAILED = 0x911Du;
// Target part of ReShade's resource handle ((target << 40) | name) for the swap chain's framebuffer 0.
constexpr unsigned int FRAMEBUFFER_DEFAULT = 0x8218u;

}  // namespace gl_enum

struct OpenGLFunctions {
  decltype(&wglGetCurrentContext) get_current_context = nullptr;
  void(APIENTRY* get_integerv)(unsigned int, int*) = nullptr;
  void(APIENTRY* get_integeri_v)(unsigned int, unsigned int, int*) = nullptr;
  unsigned char(APIENTRY* is_enabled)(unsigned int) = nullptr;
  void(APIENTRY* enable)(unsigned int) = nullptr;
  unsigned int(APIENTRY* get_error)() = nullptr;
  void(APIENTRY* get_framebuffer_attachment_parameteriv)(unsigned int, unsigned int, unsigned int, int*) = nullptr;
  const unsigned char*(APIENTRY* get_stringi)(unsigned int, unsigned int) = nullptr;
  void(APIENTRY* read_buffer)(unsigned int) = nullptr;
  void(APIENTRY* bind_framebuffer)(unsigned int, unsigned int) = nullptr;
  void(APIENTRY* memory_barrier)(unsigned int) = nullptr;
  void(APIENTRY* bind_image_texture)(unsigned int, unsigned int, int, unsigned char, int, unsigned int, unsigned int) = nullptr;
  void*(APIENTRY* fence_sync)(unsigned int, unsigned int) = nullptr;
  unsigned int(APIENTRY* client_wait_sync)(void*, unsigned int, std::uint64_t) = nullptr;
  void(APIENTRY* delete_sync)(void*) = nullptr;
};

// The entry points, or null when the process has no complete OpenGL 4.3 set. Loading needs a current context, so a
// failure is not cached; the pointers themselves are valid on any context of the process.
[[nodiscard]] inline const OpenGLFunctions* GetOpenGLFunctions() {
  static std::shared_mutex mutex;
  static std::optional<OpenGLFunctions> functions;
  {
    std::shared_lock lock(mutex);
    if (functions.has_value()) return &functions.value();
  }
  std::unique_lock lock(mutex);
  if (functions.has_value()) return &functions.value();

  auto* const module = GetModuleHandleW(L"opengl32.dll");
  if (module == nullptr) return nullptr;
  const auto get_proc_address = reinterpret_cast<decltype(&wglGetProcAddress)>(GetProcAddress(module, "wglGetProcAddress"));
  if (get_proc_address == nullptr) return nullptr;

  // OpenGL 1.1 entry points are opengl32 exports; the rest come from the driver through wglGetProcAddress.
  const auto load = [&](auto* function, const char* name, bool exported) {
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(get_proc_address(name));
    if (exported) {
      address = reinterpret_cast<std::uintptr_t>(GetProcAddress(module, name));
    }
    const auto is_valid = (address > 3u && address != (std::numeric_limits<std::uintptr_t>::max)());
    *function = (is_valid ? reinterpret_cast<std::remove_reference_t<decltype(*function)>>(address) : nullptr);
    return *function != nullptr;
  };

  OpenGLFunctions loaded;
  const bool complete =
      load(&loaded.get_current_context, "wglGetCurrentContext", true)
      && load(&loaded.get_integerv, "glGetIntegerv", true)
      && load(&loaded.is_enabled, "glIsEnabled", true)
      && load(&loaded.enable, "glEnable", true)
      && load(&loaded.get_error, "glGetError", true)
      && load(&loaded.read_buffer, "glReadBuffer", true)
      && load(&loaded.get_integeri_v, "glGetIntegeri_v", false)
      && load(&loaded.get_stringi, "glGetStringi", false)
      && load(&loaded.get_framebuffer_attachment_parameteriv, "glGetFramebufferAttachmentParameteriv", false)
      && load(&loaded.bind_framebuffer, "glBindFramebuffer", false)
      && load(&loaded.memory_barrier, "glMemoryBarrier", false)
      && load(&loaded.bind_image_texture, "glBindImageTexture", false)
      && load(&loaded.fence_sync, "glFenceSync", false)
      && load(&loaded.client_wait_sync, "glClientWaitSync", false)
      && load(&loaded.delete_sync, "glDeleteSync", false);
  if (!complete) return nullptr;

  functions = loaded;
  return &functions.value();
}

// The readback fence of a GL watch: one GL sync object per signal, created on the presenting thread's context and polled
// there (no worker thread and no extra context, unlike gpu_fence::OpenGLFence, whose thread has to be retired outside
// ReShade's WGL locks). A sync object belongs to the context it was created on: it is deleted only while that context is
// current and leaked otherwise, because a delete on another context could hit the game's own objects. Held by
// FenceTimeline, which keeps the completed value in its FenceCompletion. Not locked: every access is on the presenting
// thread with the session's context current.
struct OpenGLSyncTimeline {
  struct PendingSync {
    std::uint64_t value = 0u;
    void* sync = nullptr;
  };

  const OpenGLFunctions* functions = nullptr;
  void* context = nullptr;
  std::deque<PendingSync> pending;
  std::uint64_t last_signalled = 0u;

  OpenGLSyncTimeline() = default;
  OpenGLSyncTimeline(const OpenGLSyncTimeline&) = delete;
  OpenGLSyncTimeline& operator=(const OpenGLSyncTimeline&) = delete;
  ~OpenGLSyncTimeline() {
    if (pending.empty() || functions == nullptr || functions->get_current_context() != context) return;
    for (const auto& pending_sync : pending) {
      functions->delete_sync(pending_sync.sync);
    }
  }
};

// Per-session OpenGL state. The atlas is Session::scratch, the stats image Session::stats_texture.
struct OpenGLReduction {
  const OpenGLFunctions* functions = nullptr;
  // The context that presents the watched swap chain; every GL object and command of the watch belongs to it, and hooks
  // that run on another context (loader threads, helper windows) are ignored.
  void* context = nullptr;
  reshade::api::pipeline pipeline = {0u};
  reshade::api::resource_view atlas_view = {0u};
  reshade::api::resource_view stats_view = {0u};
  // The image units the reduction binds: the highest ones, saved and restored around the pass.
  std::uint32_t atlas_unit = 0u;
  std::uint32_t stats_unit = 0u;
  std::uint32_t max_image_units = 0u;
  std::uint32_t max_draw_buffers = 0u;
  std::uint32_t atlas_columns = 1u;
  bool compatibility_context = false;
  bool watches_default_framebuffer = false;
  // The default framebuffer's read buffer at the start of the watch: ReShade's copy from it leaves GL_BACK set.
  int default_read_buffer = 0;
  // Pending GL errors are drained once before the first copy and before the first reduction, so that an error after
  // either is the watch's own.
  bool first_copy_checked = false;
  bool first_reduction_checked = false;
};

// Clears the pending GL errors (at most 16; the game's own pending errors go with them, see DEVKIT_MCP.md).
inline void DrainOpenGLErrors(const OpenGLFunctions& functions) {
  std::uint32_t drained = 0u;
  while (drained < 16u && functions.get_error() != 0u) {
    ++drained;
  }
}

// ReShade's copy from the default framebuffer sets its read buffer to GL_BACK and does not put it back: restores the game's
// value (recorded at the start of the watch) when it differed.
inline void RestoreDefaultReadBuffer(const OpenGLReduction& opengl) {
  if (!opengl.watches_default_framebuffer || opengl.default_read_buffer == static_cast<int>(gl_enum::BACK)) return;
  const auto& functions = *opengl.functions;
  int read_framebuffer = 0;
  functions.get_integerv(gl_enum::READ_FRAMEBUFFER_BINDING, &read_framebuffer);
  functions.bind_framebuffer(gl_enum::READ_FRAMEBUFFER, 0u);
  functions.read_buffer(static_cast<unsigned int>(opengl.default_read_buffer));
  functions.bind_framebuffer(gl_enum::READ_FRAMEBUFFER, static_cast<unsigned int>(read_framebuffer));
}

}  // namespace renodx::addons::devkit::mcp::watch_region
