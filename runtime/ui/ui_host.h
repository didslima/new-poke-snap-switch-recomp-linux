// SPDX-License-Identifier: MIT
// Neutral platform host interface for runtime/ui (Stage 6F).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct ImGuiIO;

namespace snap::ui::host {

bool init_platform(void* native_window);
void new_frame(ImGuiIO& io, float width, float height);
inline void scale_window_pos_to_display(float wx, float wy, int win_w, int win_h,
                                        float disp_w, float disp_h, float* out_x, float* out_y) {
  const float sx = (win_w > 0 && disp_w > 0.0f) ? (disp_w / static_cast<float>(win_w)) : 1.0f;
  const float sy = (win_h > 0 && disp_h > 0.0f) ? (disp_h / static_cast<float>(win_h)) : 1.0f;
  if (out_x) *out_x = wx * sx;
  if (out_y) *out_y = wy * sy;
}
void shutdown_platform();
void release_cursor_confine();

std::string system_font_path(const char* filename);
uint64_t current_process_id();
char path_sep();

void play_wav_async(const void* wav_bytes, size_t size);
std::string documents_dir();
std::string exe_dir();
bool beside_exe(const char* rel_path);
bool benchmark_launch_from_game();
bool tcp_port_open(const char* ipv4_addr, unsigned short port);
void sleep_ms(unsigned ms);
void open_folder(const std::string& dir);
void open_url(const std::string& url);
bool launch_gallery_server(const std::string& root, const std::string& profile, const std::string& port_file);

}  // namespace snap::ui::host
