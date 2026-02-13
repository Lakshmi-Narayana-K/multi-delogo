/*
 * Copyright (C) 2018-2025 Werner Turing <werner.turing@protonmail.com>
 *
 * This file is part of multi-delogo.
 *
 * multi-delogo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * multi-delogo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with multi-delogo.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
 
 #include <opencv2/videoio.hpp>
 #include <opencv2/imgcodecs.hpp>
 #include <opencv2/imgproc.hpp>
 
 #include "filter-generator/FilterData.hpp"
 #include "filter-generator/FilterFactory.hpp"
 #include "filter-generator/VideoLayoutConfig.hpp"
 #include "filter-generator/RegularScriptGenerator.hpp"
 
 
struct DetectionResult {
  bool found;
  int x;
  int y;
  int width;
  int height;
  double confidence;
  int start_frame;
  int end_frame;
  std::string replacement_image;
  double scale;
  std::string name;
  bool full_screen;               // from config: this segment defines full-screen ranges
  bool suppress_during_full_screen;  // from config: do not draw during full_screen segments
  bool is_moving;                 // NEW: flag indicating if logo moves
  std::vector<std::pair<int, std::pair<int, int>>> tracked_positions;  // NEW: (frame_num, (x, y)) for moving logos
};
 
 
void print_usage(const char* program_name)
{
  std::cout << "Usage: " << program_name << " [OPTIONS]\n"
            << "\n"
            << "Batch process videos with automatic logo detection and replacement.\n"
            << "\n"
            << "Options:\n"
            << "  --input-folder PATH     Input folder containing videos\n"
            << "  --output-folder PATH    Output folder for processed videos\n"
            << "  --config PATH           Path to video_layouts.json config file\n"
            << "  --layout ID             Layout ID to use (or 'auto' for auto-detect)\n"
            << "  --auto-detect           Auto-detect layout by video resolution\n"
            << "  --ffmpeg PATH           Path to ffmpeg executable (default: ffmpeg)\n"
            << "  --sample-interval N     Check every Nth frame for detection (default: 30)\n"
            << "  --track-moving          Enable frame-by-frame tracking for moving logos\n"
            << "  --dry-run               Show what would be done without executing\n"
            << "  --help                  Show this help message\n"
            << "\n"
            << "Example:\n"
            << "  " << program_name << " --input-folder ./videos --output-folder ./output \\\n"
            << "                     --config video_layouts.json --auto-detect\n"
            << "  " << program_name << " --input-folder ./videos --output-folder ./output \\\n"
            << "                     --config video_layouts.json --auto-detect --track-moving\n"
            << std::endl;
}
 
 
 bool is_video_file(const std::string& filename)
 {
   std::vector<std::string> extensions = {
     ".mp4", ".avi", ".mkv", ".mov", ".wmv", ".flv", ".webm", ".m4v", ".mpeg", ".mpg"
   };
   
   for (const auto& ext : extensions) {
     if (filename.size() >= ext.size() &&
         filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0) {
       return true;
     }
   }
   return false;
 }
 
 
 std::vector<std::string> list_video_files(const std::string& folder)
 {
   std::vector<std::string> videos;
   
   DIR* dir = opendir(folder.c_str());
   if (!dir) {
     std::cerr << "Error: Cannot open folder " << folder << std::endl;
     return videos;
   }
   
   struct dirent* entry;
   while ((entry = readdir(dir)) != nullptr) {
     std::string filename = entry->d_name;
     if (is_video_file(filename)) {
       videos.push_back(folder + "/" + filename);
     }
   }
   
   closedir(dir);
   
   std::sort(videos.begin(), videos.end());
   return videos;
 }
 
 
 std::string get_output_filename(const std::string& input_path, const std::string& output_folder)
 {
   size_t pos = input_path.rfind('/');
   std::string filename = (pos != std::string::npos) ? input_path.substr(pos + 1) : input_path;
   
   // Add "_processed" before the extension
   size_t ext_pos = filename.rfind('.');
   if (ext_pos != std::string::npos) {
     filename = filename.substr(0, ext_pos) + "_processed" + filename.substr(ext_pos);
   } else {
     filename += "_processed";
   }
   
   return output_folder + "/" + filename;
 }
 
 
 bool get_video_info(const std::string& video_path, int& width, int& height, int& frame_count, double& fps)
 {
   cv::VideoCapture cap(video_path);
   if (!cap.isOpened()) {
     return false;
   }
   
   width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
   height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
   frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
   fps = cap.get(cv::CAP_PROP_FPS);
   
  cap.release();
  return true;
}


// NEW: Process video with moving logo tracking - applies overlays frame-by-frame at tracked positions
bool process_video_with_moving_logos(
    const std::string& input_path,
    const std::string& output_path,
    const std::vector<DetectionResult>& detections,
    fg::VideoLayoutManager& layout_manager,
    double fps)
{
  const int LEFT_EXIT_WHITE_W = 300;
  const int LEFT_EXIT_WHITE_H = 144;
  const int LEFT_EXIT_WHITE_TAIL_FRAMES = 5;
  const int MAX_HOLD_LAST_POSITION_FRAMES = 6;

  auto moving_mode_start = std::chrono::steady_clock::now();
  std::cout << "  [MOVING MODE] Processing video frame-by-frame..." << std::endl;
  
  cv::VideoCapture cap(input_path);
  if (!cap.isOpened()) {
    std::cerr << "  Error: Cannot open input video" << std::endl;
    return false;
  }
  
  int frame_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int frame_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  int frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  
  // Setup video writer
  cv::VideoWriter writer(output_path, 
                         cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                         fps,
                         cv::Size(frame_width, frame_height));
  
  if (!writer.isOpened()) {
    std::cerr << "  Error: Cannot create output video writer" << std::endl;
    cap.release();
    return false;
  }
  
  // Load all replacement images
  std::map<std::string, cv::Mat> replacement_images;
  for (const auto& det : detections) {
    if (replacement_images.find(det.replacement_image) == replacement_images.end()) {
      std::string img_path = layout_manager.get_image_path(det.replacement_image);
      cv::Mat img = cv::imread(img_path, cv::IMREAD_UNCHANGED);
      if (img.empty()) {
        std::cerr << "  Error: Cannot load replacement image: " << img_path << std::endl;
        cap.release();
        writer.release();
        return false;
      }
      replacement_images[det.replacement_image] = img;
      std::cout << "  Loaded replacement image: " << det.replacement_image << std::endl;
    }
  }

  // White patch used for a short post-exit cleanup when a logo leaves from the left edge.
  cv::Mat left_exit_white_overlay = cv::imread(layout_manager.get_reference_path("white.png"), cv::IMREAD_UNCHANGED);
  if (left_exit_white_overlay.empty()) {
    std::cerr << "  Warning: Cannot load reference_images/white.png, using generated white patch fallback" << std::endl;
    left_exit_white_overlay = cv::Mat(LEFT_EXIT_WHITE_H, LEFT_EXIT_WHITE_W, CV_8UC3, cv::Scalar(255, 255, 255));
  }
  
  // Create position lookup maps for each detection (frame_num -> position)
  std::vector<std::map<int, std::pair<int, int>>> position_maps(detections.size());
  for (size_t i = 0; i < detections.size(); ++i) {
    for (const auto& pos : detections[i].tracked_positions) {
      position_maps[i][pos.first] = pos.second;
    }
  }

  // Per-detection state for short white tail after left-edge exits.
  std::vector<bool> left_exit_white_enabled(detections.size(), false);
  std::vector<std::pair<int, int>> left_exit_white_anchor(detections.size(), {0, 0});
  std::vector<int> left_exit_last_tracked_frame(detections.size(), -1);
  
  // Pre-compute effective start/end for each detection
  // For moving logos, extend range to cover partial visibility when entering/exiting screen
  std::vector<std::pair<int, int>> effective_range(detections.size());
  for (size_t i = 0; i < detections.size(); ++i) {
    const auto& det = detections[i];
    const auto& pm = position_maps[i];
    effective_range[i] = {det.start_frame, det.end_frame};
    if (!pm.empty()) {
      left_exit_last_tracked_frame[i] = pm.rbegin()->first;
    }
    
    if (det.is_moving && pm.size() >= 2) {
      int scaled_w = static_cast<int>(det.width * det.scale);
      int scaled_h = static_cast<int>(det.height * det.scale);
      int logo_dim = std::max(scaled_w, scaled_h);
      const int exit_tail_guard_frames = 8;  // Keep drawing a few extra frames while logo exits
      
      // Velocity at start (for backward extension — logo entering the screen)
      {
        auto first = pm.begin();
        auto second = first; ++second;
        int df = second->first - first->first;
        if (df > 0) {
          double speed = std::sqrt(
            std::pow(static_cast<double>(second->second.first - first->second.first), 2.0) +
            std::pow(static_cast<double>(second->second.second - first->second.second), 2.0)) / df;
          if (speed > 0.5) {
            int pad = std::min(static_cast<int>(std::ceil(logo_dim / speed)) + exit_tail_guard_frames, 90);
            effective_range[i].first = std::max(0, det.start_frame - pad);
          }
        }
      }
      
      // Velocity at end (for forward extension — logo exiting the screen)
      {
        auto last = pm.end(); --last;
        auto second_last = last; --second_last;
        int df = last->first - second_last->first;
        if (df > 0) {
          double speed = std::sqrt(
            std::pow(static_cast<double>(last->second.first - second_last->second.first), 2.0) +
            std::pow(static_cast<double>(last->second.second - second_last->second.second), 2.0)) / df;
          if (speed > 0.5) {
            int pad = std::min(static_cast<int>(std::ceil(logo_dim / speed)) + exit_tail_guard_frames, 90);
            effective_range[i].second = std::min(frame_count - 1, det.end_frame + pad);
          }
        }
      }
      
      if (effective_range[i].first != det.start_frame || effective_range[i].second != det.end_frame) {
        std::cout << "  [" << det.name << "] Extended range for partial visibility: "
                  << det.start_frame << "-" << det.end_frame << " → "
                  << effective_range[i].first << "-" << effective_range[i].second << std::endl;
      }
    }

    if (det.is_moving && pm.size() >= 2) {
      auto last = pm.end();
      --last;
      auto prev = last;
      --prev;
      int df = last->first - prev->first;
      if (df > 0) {
        double vx = static_cast<double>(last->second.first - prev->second.first) / df;
        if (vx < -0.5) {
          left_exit_white_enabled[i] = true;
          int anchor_x = std::max(0, last->second.first);
          int anchor_y = std::max(0, last->second.second);
          left_exit_white_anchor[i] = {anchor_x, anchor_y};
        }
      }
    }
  }
  
  // Process each frame
  cv::Mat frame;
  int processed_frames = 0;
  std::vector<long long> interval_time_us(detections.size(), 0);
  std::vector<int> interval_in_range_frames(detections.size(), 0);
  std::vector<int> interval_drawn_frames(detections.size(), 0);
  std::vector<int> last_drawn_frame(detections.size(), -1000000);
  std::vector<std::pair<int, int>> last_drawn_xy(detections.size(), {0, 0});
  long long read_time_us = 0;
  long long write_time_us = 0;
  long long frame_loop_total_us = 0;
  
  for (int frame_num = 0; frame_num < frame_count; ++frame_num) {
    auto frame_t0 = std::chrono::steady_clock::now();
    auto read_t0 = frame_t0;
    if (!cap.read(frame)) {
      std::cerr << "  Warning: Could not read frame " << frame_num << std::endl;
      break;
    }
    read_time_us += std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - read_t0).count();
    
    // Apply overlays for each detection at tracked positions
    for (size_t det_idx = 0; det_idx < detections.size(); ++det_idx) {
      const auto& det = detections[det_idx];
      const auto& pos_map = position_maps[det_idx];

      bool in_effective_range = frame_num >= effective_range[det_idx].first &&
                                frame_num <= effective_range[det_idx].second;
      bool in_left_exit_white_tail = left_exit_white_enabled[det_idx] &&
                                     frame_num > left_exit_last_tracked_frame[det_idx] &&
                                     frame_num <= left_exit_last_tracked_frame[det_idx] + LEFT_EXIT_WHITE_TAIL_FRAMES;
      if (!in_effective_range && !in_left_exit_white_tail) {
        continue;
      }
      auto interval_t0 = std::chrono::steady_clock::now();
      interval_in_range_frames[det_idx]++;

      // After a confirmed left-edge exit, do not keep drawing this detection indefinitely
      // in the extended effective range; only allow a short cleanup tail window.
      if (left_exit_white_enabled[det_idx] &&
          left_exit_last_tracked_frame[det_idx] >= 0 &&
          frame_num > left_exit_last_tracked_frame[det_idx] + LEFT_EXIT_WHITE_TAIL_FRAMES) {
        interval_time_us[det_idx] += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - interval_t0).count();
        continue;
      }

      if (in_left_exit_white_tail) {
        cv::Mat scaled_white;
        cv::resize(left_exit_white_overlay, scaled_white, cv::Size(LEFT_EXIT_WHITE_W, LEFT_EXIT_WHITE_H));

        int src_x = 0, src_y = 0;
        int dst_x = 0;
        int dst_y = left_exit_white_anchor[det_idx].second;
        int vis_w = LEFT_EXIT_WHITE_W, vis_h = LEFT_EXIT_WHITE_H;

        if (dst_x < 0) {
          src_x = -dst_x;
          vis_w += dst_x;
          dst_x = 0;
        }
        if (dst_y < 0) {
          src_y = -dst_y;
          vis_h += dst_y;
          dst_y = 0;
        }
        if (dst_x + vis_w > frame_width) {
          vis_w = frame_width - dst_x;
        }
        if (dst_y + vis_h > frame_height) {
          vis_h = frame_height - dst_y;
        }

        if (vis_w > 0 && vis_h > 0) {
          interval_drawn_frames[det_idx]++;
          cv::Mat visible_part = scaled_white(cv::Rect(src_x, src_y, vis_w, vis_h));
          if (visible_part.channels() == 4) {
            std::vector<cv::Mat> channels;
            cv::split(visible_part, channels);
            std::vector<cv::Mat> bgr_channels = {channels[0], channels[1], channels[2]};
            cv::Mat bgr;
            cv::merge(bgr_channels, bgr);

            cv::Mat alpha_3ch;
            cv::Mat alpha_channels[] = {channels[3], channels[3], channels[3]};
            cv::merge(alpha_channels, 3, alpha_3ch);
            alpha_3ch.convertTo(alpha_3ch, CV_32F, 1.0 / 255.0);

            cv::Mat roi = frame(cv::Rect(dst_x, dst_y, vis_w, vis_h));
            cv::Mat roi_float, bgr_float;
            roi.convertTo(roi_float, CV_32F);
            bgr.convertTo(bgr_float, CV_32F);
            cv::Mat blended = bgr_float.mul(alpha_3ch) + roi_float.mul(cv::Scalar(1.0, 1.0, 1.0) - alpha_3ch);
            blended.convertTo(roi, CV_8U);
          } else {
            visible_part.copyTo(frame(cv::Rect(dst_x, dst_y, vis_w, vis_h)));
          }
        }

        interval_time_us[det_idx] += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - interval_t0).count();
        continue;
      }
      
      // Find the position to use for this frame
      int x = 0, y = 0;
      bool have_position = true;
      if (pos_map.empty()) {
        // Static detection — no tracked positions, use fixed (x, y) from detection
        x = det.x;
        y = det.y;
      } else {
        auto it = pos_map.find(frame_num);
        if (it != pos_map.end()) {
          // Exact position available for this frame
          x = it->second.first;
          y = it->second.second;
        } else {
          // Linear interpolation between closest previous and next known positions
          auto next_it = pos_map.lower_bound(frame_num);  // first >= frame_num
          auto prev_it = next_it;
          bool has_prev = false, has_next = false;
          
          if (prev_it != pos_map.begin()) {
            --prev_it;
            has_prev = true;
          }
          if (next_it != pos_map.end()) {
            has_next = true;
          }
          
          // Helper lambda: extrapolate forward from prev using velocity averaged from up to 5 nearby pairs
          auto extrapolate_forward = [&](decltype(prev_it) pit, int target_frame) -> std::pair<int,int> {
            double vx = 0, vy = 0;
            int count = 0;
            auto it = pit;
            for (int i = 0; i < 5 && it != pos_map.begin(); ++i) {
              auto prev = it;
              --prev;
              int df = it->first - prev->first;
              if (df > 0 && df <= 5) {  // Only use pairs within 5 frames of each other
                vx += static_cast<double>(it->second.first - prev->second.first) / df;
                vy += static_cast<double>(it->second.second - prev->second.second) / df;
                count++;
              } else if (df > 5) {
                break;  // Stop at large gaps
              }
              it = prev;
            }
            if (count > 0) {
              vx /= count;
              vy /= count;
              int dt = target_frame - pit->first;
              return {pit->second.first + static_cast<int>(std::round(vx * dt)),
                      pit->second.second + static_cast<int>(std::round(vy * dt))};
            }
            return pit->second;
          };
          
          // Helper lambda: extrapolate backward from next using velocity averaged from up to 5 nearby pairs
          auto extrapolate_backward = [&](decltype(next_it) nit, int target_frame) -> std::pair<int,int> {
            double vx = 0, vy = 0;
            int count = 0;
            auto it = nit;
            for (int i = 0; i < 5; ++i) {
              auto next = it;
              ++next;
              if (next == pos_map.end()) break;
              int df = next->first - it->first;
              if (df > 0 && df <= 5) {
                vx += static_cast<double>(next->second.first - it->second.first) / df;
                vy += static_cast<double>(next->second.second - it->second.second) / df;
                count++;
              } else if (df > 5) {
                break;
              }
              it = next;
            }
            if (count > 0) {
              vx /= count;
              vy /= count;
              int dt = target_frame - nit->first;  // negative
              return {nit->second.first + static_cast<int>(std::round(vx * dt)),
                      nit->second.second + static_cast<int>(std::round(vy * dt))};
            }
            return nit->second;
          };
          
          if (has_prev && has_next) {
            int pf = prev_it->first, nf = next_it->first;
            int px = prev_it->second.first, py = prev_it->second.second;
            int nx = next_it->second.first, ny = next_it->second.second;
            
            // Detect discontinuity: check if the jump between prev and next
            // is consistent with the velocity at prev (logo went off-screen and came back)
            bool is_discontinuity = false;
            int frame_gap = nf - pf;
            auto prev2_check = prev_it;
            if (prev2_check != pos_map.begin()) {
              --prev2_check;
              int df = prev_it->first - prev2_check->first;
              if (df > 0) {
                double local_vx = static_cast<double>(px - prev2_check->second.first) / df;
                double local_vy = static_cast<double>(py - prev2_check->second.second) / df;
                // Where would the logo be at nf if it continued at local velocity?
                double expected_x = px + local_vx * frame_gap;
                double expected_y = py + local_vy * frame_gap;
                double deviation = std::sqrt((nx - expected_x) * (nx - expected_x) +
                                             (ny - expected_y) * (ny - expected_y));
                // If actual next pos is far from expected, it's a discontinuity
                // (logo went off-screen and reappeared elsewhere)
                double logo_size = std::max(det.width, det.height) * det.scale;
                if (deviation > logo_size * 0.5) {
                  is_discontinuity = true;
                }
              }
            }
            
            if (!is_discontinuity) {
              // Smooth movement — interpolate linearly
              double t = (nf != pf) ? static_cast<double>(frame_num - pf) / (nf - pf) : 0.0;
              x = px + static_cast<int>(std::round(t * (nx - px)));
              y = py + static_cast<int>(std::round(t * (ny - py)));
            } else {
              // Discontinuity: logo went off-screen and came back at different position
              // Try both directions; prefer exit trajectory if on-screen, else use re-entry
              // Use the same draw padding as overlay rendering, so visibility checks
              // don't cut coverage early near frame edges.
              int draw_pad = det.is_moving ? 4 : 0;
              int scaled_w = static_cast<int>(det.width * det.scale) + 2 * draw_pad;
              int scaled_h = static_cast<int>(det.height * det.scale) + 2 * draw_pad;
              
              auto fwd_pos = extrapolate_forward(prev_it, frame_num);
              auto bwd_pos = extrapolate_backward(next_it, frame_num);
              
              // Check if each extrapolated position would be at least partially on-screen
              bool fwd_visible = (fwd_pos.first + scaled_w > 0 && fwd_pos.first < frame_width &&
                                  fwd_pos.second + scaled_h > 0 && fwd_pos.second < frame_height);
              bool bwd_visible = (bwd_pos.first + scaled_w > 0 && bwd_pos.first < frame_width &&
                                  bwd_pos.second + scaled_h > 0 && bwd_pos.second < frame_height);
              
              int dt_to_prev = frame_num - pf;
              int dt_to_next = nf - frame_num;
              
              if (fwd_visible && !bwd_visible) {
                // Only exit trajectory visible (logo still exiting)
                x = fwd_pos.first;  y = fwd_pos.second;
              } else if (bwd_visible && !fwd_visible) {
                // Only re-entry trajectory visible (exit went off-screen, logo coming back)
                x = bwd_pos.first;  y = bwd_pos.second;
              } else {
                // Both visible, both off-screen, or any combo — use the time-closer one.
                // NEVER skip: the clipping code safely handles off-screen positions
                // (draws nothing if fully off-screen, draws visible portion if partially on-screen).
                // This eliminates blink when our extrapolation is slightly off.
                if (dt_to_prev <= dt_to_next) {
                  x = fwd_pos.first;  y = fwd_pos.second;
                } else {
                  x = bwd_pos.first;  y = bwd_pos.second;
                }
              }
            }
          } else if (has_prev) {
            // Past end of tracked data — extrapolate forward (logo going off-screen)
            auto pos = extrapolate_forward(prev_it, frame_num);
            x = pos.first;
            y = pos.second;
          } else if (has_next) {
            // Before start of tracked data — extrapolate backward (logo entering screen)
            auto pos = extrapolate_backward(next_it, frame_num);
            x = pos.first;
            y = pos.second;
          } else {
            have_position = false;  // No tracked position nearby
          }
        }
      }

      // Short fallback for brief tracking gaps: keep drawing at last good position.
      // Restrict to core interval and a tiny frame window to avoid long-video ghosts.
      if (!have_position) {
        bool allow_hold = det.is_moving &&
                          frame_num >= det.start_frame &&
                          frame_num <= det.end_frame &&
                          (frame_num - last_drawn_frame[det_idx]) > 0 &&
                          (frame_num - last_drawn_frame[det_idx]) <= MAX_HOLD_LAST_POSITION_FRAMES;
        if (allow_hold) {
          x = last_drawn_xy[det_idx].first;
          y = last_drawn_xy[det_idx].second;
        } else {
          interval_time_us[det_idx] += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - interval_t0).count();
          continue;
        }
      }
      
      // Now apply the overlay at position (x, y)
      // For left-edge exits, force a white patch once the tracked box crosses x<0.
      // This prevents the old logo tail from peeking through during partial visibility.
      bool in_left_exit_window = left_exit_last_tracked_frame[det_idx] >= 0 &&
                                 frame_num >= std::max(det.start_frame, left_exit_last_tracked_frame[det_idx] - 2) &&
                                 frame_num <= left_exit_last_tracked_frame[det_idx] + LEFT_EXIT_WHITE_TAIL_FRAMES;
      if (left_exit_white_enabled[det_idx] && in_left_exit_window && x < 0) {
        cv::Mat scaled_white;
        cv::resize(left_exit_white_overlay, scaled_white, cv::Size(LEFT_EXIT_WHITE_W, LEFT_EXIT_WHITE_H));

        int src_x = 0, src_y = 0;
        int dst_x = 0;
        int dst_y = left_exit_white_anchor[det_idx].second;
        int vis_w = LEFT_EXIT_WHITE_W, vis_h = LEFT_EXIT_WHITE_H;

        if (dst_x < 0) {
          src_x = -dst_x;
          vis_w += dst_x;
          dst_x = 0;
        }
        if (dst_y < 0) {
          src_y = -dst_y;
          vis_h += dst_y;
          dst_y = 0;
        }
        if (dst_x + vis_w > frame_width) {
          vis_w = frame_width - dst_x;
        }
        if (dst_y + vis_h > frame_height) {
          vis_h = frame_height - dst_y;
        }

        if (vis_w > 0 && vis_h > 0) {
          interval_drawn_frames[det_idx]++;
          cv::Mat visible_part = scaled_white(cv::Rect(src_x, src_y, vis_w, vis_h));
          if (visible_part.channels() == 4) {
            std::vector<cv::Mat> channels;
            cv::split(visible_part, channels);
            std::vector<cv::Mat> bgr_channels = {channels[0], channels[1], channels[2]};
            cv::Mat bgr;
            cv::merge(bgr_channels, bgr);

            cv::Mat alpha_3ch;
            cv::Mat alpha_channels[] = {channels[3], channels[3], channels[3]};
            cv::merge(alpha_channels, 3, alpha_3ch);
            alpha_3ch.convertTo(alpha_3ch, CV_32F, 1.0 / 255.0);

            cv::Mat roi = frame(cv::Rect(dst_x, dst_y, vis_w, vis_h));
            cv::Mat roi_float, bgr_float;
            roi.convertTo(roi_float, CV_32F);
            bgr.convertTo(bgr_float, CV_32F);
            cv::Mat blended = bgr_float.mul(alpha_3ch) + roi_float.mul(cv::Scalar(1.0, 1.0, 1.0) - alpha_3ch);
            blended.convertTo(roi, CV_8U);
          } else {
            visible_part.copyTo(frame(cv::Rect(dst_x, dst_y, vis_w, vis_h)));
          }
        }

        interval_time_us[det_idx] += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - interval_t0).count();
        continue;
      }

      // Get replacement image
      cv::Mat replacement = replacement_images[det.replacement_image];
      
      // For moving logos, add small padding (2px each side) to cover sub-pixel
      // position estimation errors that could let the old logo peek through edges
      int pad = det.is_moving ? 4 : 0;
      x -= pad;
      y -= pad;
      
      // Scale replacement image (with padding for moving logos)
      int scaled_w = static_cast<int>(det.width * det.scale) + 2 * pad;
      int scaled_h = static_cast<int>(det.height * det.scale) + 2 * pad;
      cv::Mat scaled_replacement;
      cv::resize(replacement, scaled_replacement, cv::Size(scaled_w, scaled_h));
      
      // Clip overlay to the visible portion of the frame (handles logos partially off-screen)
      int src_x = 0, src_y = 0;  // top-left corner in the replacement image
      int dst_x = x, dst_y = y;  // top-left corner in the frame
      int vis_w = scaled_w, vis_h = scaled_h;

      // Clip left edge
      if (dst_x < 0) {
        src_x = -dst_x;
        vis_w += dst_x;
        dst_x = 0;
      }
      // Clip top edge
      if (dst_y < 0) {
        src_y = -dst_y;
        vis_h += dst_y;
        dst_y = 0;
      }
      // Clip right edge
      if (dst_x + vis_w > frame_width) {
        vis_w = frame_width - dst_x;
      }
      // Clip bottom edge
      if (dst_y + vis_h > frame_height) {
        vis_h = frame_height - dst_y;
      }

      // Only draw if there's a visible region
      if (vis_w > 0 && vis_h > 0) {
        interval_drawn_frames[det_idx]++;
        last_drawn_frame[det_idx] = frame_num;
        last_drawn_xy[det_idx] = {x, y};
        // Crop the scaled replacement to the visible portion
        cv::Mat visible_part = scaled_replacement(cv::Rect(src_x, src_y, vis_w, vis_h));

        // Handle alpha channel if present
        if (visible_part.channels() == 4) {
          std::vector<cv::Mat> channels;
          cv::split(visible_part, channels);

          std::vector<cv::Mat> bgr_channels = {channels[0], channels[1], channels[2]};
          cv::Mat bgr;
          cv::merge(bgr_channels, bgr);

          cv::Mat alpha_3ch;
          cv::Mat alpha_channels[] = {channels[3], channels[3], channels[3]};
          cv::merge(alpha_channels, 3, alpha_3ch);

          alpha_3ch.convertTo(alpha_3ch, CV_32F, 1.0/255.0);

          cv::Mat roi = frame(cv::Rect(dst_x, dst_y, vis_w, vis_h));
          cv::Mat roi_float, bgr_float;
          roi.convertTo(roi_float, CV_32F);
          bgr.convertTo(bgr_float, CV_32F);

          cv::Mat blended = bgr_float.mul(alpha_3ch) + roi_float.mul(cv::Scalar(1.0, 1.0, 1.0) - alpha_3ch);
          blended.convertTo(roi, CV_8U);
        } else {
          visible_part.copyTo(frame(cv::Rect(dst_x, dst_y, vis_w, vis_h)));
        }
      } else {
        bool allow_hold = det.is_moving &&
                          !left_exit_white_enabled[det_idx] &&
                          frame_num >= det.start_frame &&
                          frame_num <= det.end_frame &&
                          (frame_num - last_drawn_frame[det_idx]) > 0 &&
                          (frame_num - last_drawn_frame[det_idx]) <= MAX_HOLD_LAST_POSITION_FRAMES;
        if (allow_hold) {
          int hold_x = last_drawn_xy[det_idx].first;
          int hold_y = last_drawn_xy[det_idx].second;
          int hold_src_x = 0, hold_src_y = 0;
          int hold_dst_x = hold_x, hold_dst_y = hold_y;
          int hold_vis_w = scaled_w, hold_vis_h = scaled_h;

          if (hold_dst_x < 0) {
            hold_src_x = -hold_dst_x;
            hold_vis_w += hold_dst_x;
            hold_dst_x = 0;
          }
          if (hold_dst_y < 0) {
            hold_src_y = -hold_dst_y;
            hold_vis_h += hold_dst_y;
            hold_dst_y = 0;
          }
          if (hold_dst_x + hold_vis_w > frame_width) {
            hold_vis_w = frame_width - hold_dst_x;
          }
          if (hold_dst_y + hold_vis_h > frame_height) {
            hold_vis_h = frame_height - hold_dst_y;
          }

          if (hold_vis_w > 0 && hold_vis_h > 0) {
            interval_drawn_frames[det_idx]++;
            cv::Mat hold_part = scaled_replacement(cv::Rect(hold_src_x, hold_src_y, hold_vis_w, hold_vis_h));
            if (hold_part.channels() == 4) {
              std::vector<cv::Mat> channels;
              cv::split(hold_part, channels);

              std::vector<cv::Mat> bgr_channels = {channels[0], channels[1], channels[2]};
              cv::Mat bgr;
              cv::merge(bgr_channels, bgr);

              cv::Mat alpha_3ch;
              cv::Mat alpha_channels[] = {channels[3], channels[3], channels[3]};
              cv::merge(alpha_channels, 3, alpha_3ch);
              alpha_3ch.convertTo(alpha_3ch, CV_32F, 1.0 / 255.0);

              cv::Mat roi = frame(cv::Rect(hold_dst_x, hold_dst_y, hold_vis_w, hold_vis_h));
              cv::Mat roi_float, bgr_float;
              roi.convertTo(roi_float, CV_32F);
              bgr.convertTo(bgr_float, CV_32F);
              cv::Mat blended = bgr_float.mul(alpha_3ch) + roi_float.mul(cv::Scalar(1.0, 1.0, 1.0) - alpha_3ch);
              blended.convertTo(roi, CV_8U);
            } else {
              hold_part.copyTo(frame(cv::Rect(hold_dst_x, hold_dst_y, hold_vis_w, hold_vis_h)));
            }
            last_drawn_frame[det_idx] = frame_num;
            last_drawn_xy[det_idx] = {hold_x, hold_y};
          }
        }
      }
      interval_time_us[det_idx] += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - interval_t0).count();
    }
    
    // Write processed frame
    auto write_t0 = std::chrono::steady_clock::now();
    writer.write(frame);
    write_time_us += std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - write_t0).count();
    processed_frames++;
    frame_loop_total_us += std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - frame_t0).count();
    
    // Progress indicator
    if (frame_num % 30 == 0) {
      std::cout << "  Progress: " << frame_num << "/" << frame_count 
                << " (" << (frame_num * 100 / frame_count) << "%)\r" << std::flush;
    }
  }
  
  std::cout << std::endl;
  std::cout << "  Processed " << processed_frames << " frames" << std::endl;
  std::cout << "\n  --- Interval Processing Time Summary (OpenCV pass) ---" << std::endl;
  double total_static_sec = 0.0, total_moving_sec = 0.0;
  double total_static_ms = 0.0, total_moving_ms = 0.0;
  for (size_t i = 0; i < detections.size(); ++i) {
    const auto& det = detections[i];
    int range_frames = std::max(0, effective_range[i].second - effective_range[i].first + 1);
    double range_sec = range_frames / std::max(0.001, fps);
    double ms = interval_time_us[i] / 1000.0;
    double ms_per_frame = (interval_in_range_frames[i] > 0)
      ? (ms / interval_in_range_frames[i]) : 0.0;
    std::cout << "    [" << (det.is_moving ? "MOVING" : "STATIC") << "] " << det.name
              << " frames " << effective_range[i].first << "-" << effective_range[i].second
              << " (" << std::fixed << std::setprecision(2) << range_sec << "s)"
              << " | in-range=" << interval_in_range_frames[i]
              << " | drawn=" << interval_drawn_frames[i]
              << " | time=" << std::fixed << std::setprecision(1) << ms << "ms"
              << " | avg=" << std::fixed << std::setprecision(3) << ms_per_frame << "ms/frame"
              << std::endl;
    if (det.is_moving) {
      total_moving_sec += range_sec;
      total_moving_ms += ms;
    } else {
      total_static_sec += range_sec;
      total_static_ms += ms;
    }
  }
  std::cout << "    --------------------------------------------------------" << std::endl;
  std::cout << "    STATIC total: " << std::fixed << std::setprecision(2) << total_static_sec
            << "s intervals | " << std::fixed << std::setprecision(1) << total_static_ms << "ms"
            << " | avg " << ((total_static_sec > 0.0) ? (total_static_ms / total_static_sec) : 0.0)
            << " ms/s-video" << std::endl;
  std::cout << "    MOVING total: " << std::fixed << std::setprecision(2) << total_moving_sec
            << "s intervals | " << std::fixed << std::setprecision(1) << total_moving_ms << "ms"
            << " | avg " << ((total_moving_sec > 0.0) ? (total_moving_ms / total_moving_sec) : 0.0)
            << " ms/s-video" << std::endl;
  double interval_sum_ms = 0.0;
  for (const auto& us : interval_time_us) interval_sum_ms += us / 1000.0;
  double read_ms = read_time_us / 1000.0;
  double write_ms = write_time_us / 1000.0;
  double frame_loop_ms = frame_loop_total_us / 1000.0;
  double other_loop_ms = std::max(0.0, frame_loop_ms - (interval_sum_ms + read_ms + write_ms));
  double moving_mode_wall_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - moving_mode_start).count();
  double setup_and_finalize_ms = std::max(0.0, moving_mode_wall_ms - frame_loop_ms);
  std::cout << "\n  --- OpenCV Pass Timing Reconciliation ---" << std::endl;
  std::cout << "    Wall (process_video_with_moving_logos): " << std::fixed << std::setprecision(1)
            << moving_mode_wall_ms << "ms" << std::endl;
  std::cout << "    Frame loop total:                      " << frame_loop_ms << "ms" << std::endl;
  std::cout << "      - Overlay interval sum:              " << interval_sum_ms << "ms" << std::endl;
  std::cout << "      - Frame read (cap.read):             " << read_ms << "ms" << std::endl;
  std::cout << "      - Frame write (writer.write):        " << write_ms << "ms" << std::endl;
  std::cout << "      - Other loop overhead:               " << other_loop_ms << "ms" << std::endl;
  std::cout << "    Setup/finalize outside frame loop:     " << setup_and_finalize_ms << "ms" << std::endl;
  
  cap.release();
  writer.release();
  
  return true;
}


// Template matching to detect logo in a frame (optional multi-scale for size tolerance)
 bool detect_logo_in_frame(const cv::Mat& frame, 
                           const cv::Mat& template_img,
                           const fg::SearchRegion& search_region,
                           double threshold,
                           int& found_x, int& found_y,
                           double& confidence,
                           bool use_multi_scale)
 {
   // Extract the search region from the frame
   int roi_x = std::max(0, search_region.x);
   int roi_y = std::max(0, search_region.y);
   int roi_w = std::min(search_region.width, frame.cols - roi_x);
   int roi_h = std::min(search_region.height, frame.rows - roi_y);
   
   if (roi_w <= 0 || roi_h <= 0) {
     return false;
   }
   
   cv::Rect roi(roi_x, roi_y, roi_w, roi_h);
   cv::Mat search_area = frame(roi);
   
   double best_val = -1.0;
   cv::Point best_loc(0, 0);
   
   if (use_multi_scale) {
   const double scales[] = { 0.9, 0.95, 1.0, 1.05, 1.1 };
   const int num_scales = sizeof(scales) / sizeof(scales[0]);
   for (int s = 0; s < num_scales; ++s) {
     int tw = static_cast<int>(template_img.cols * scales[s]);
     int th = static_cast<int>(template_img.rows * scales[s]);
     if (tw < 5 || th < 5 || tw > roi_w || th > roi_h) continue;
     
     cv::Mat scaled_template;
     cv::resize(template_img, scaled_template, cv::Size(tw, th), 0, 0, cv::INTER_LINEAR);
     
     cv::Mat result;
     cv::matchTemplate(search_area, scaled_template, result, cv::TM_CCOEFF_NORMED);
     
     double min_val, max_val;
     cv::Point min_loc, max_loc;
     cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);
     
     if (max_val > best_val) {
       best_val = max_val;
       best_loc = max_loc;
     }
   }
   } else {
     cv::Mat result;
     cv::matchTemplate(search_area, template_img, result, cv::TM_CCOEFF_NORMED);
     double min_val, max_val;
     cv::Point min_loc, max_loc;
     cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);
     best_val = max_val;
     best_loc = max_loc;
   }
   
   confidence = best_val;
   
   if (best_val >= threshold) {
     found_x = roi_x + best_loc.x;
     found_y = roi_y + best_loc.y;
     return true;
   }
   
   return false;
 }
 
 
// Per-detection state for frame-first (single pass) scanning
struct DetectionState {
  cv::Mat template_img;
  fg::SearchRegion effective_region;
  double match_threshold;
  std::string replacement_image;
  double scale;
  std::string name;
  bool full_screen;
  bool suppress_during_full_screen;
  bool currently_detecting = false;
  int detection_start_frame = -1;
 int last_found_frame = -1;
  int last_detected_x = 0, last_detected_y = 0;
  int consecutive_misses = 0;
  double last_confidence = 0.0;
};


// Analyze if a logo segment is moving or static based on position changes
bool is_logo_moving(const std::vector<std::pair<int, std::pair<int, int>>>& positions, double threshold = 10.0) {
  if (positions.size() < 2) {
    return false;  // Need at least 2 samples
  }
  
  // Calculate total and max movement between consecutive samples
  double total_movement = 0.0;
  double max_single_move = 0.0;
  for (size_t i = 1; i < positions.size(); ++i) {
    int dx = positions[i].second.first - positions[i-1].second.first;
    int dy = positions[i].second.second - positions[i-1].second.second;
    double distance = std::sqrt(dx*dx + dy*dy);
    total_movement += distance;
    max_single_move = std::max(max_single_move, distance);
  }
  
  double avg_movement = total_movement / (positions.size() - 1);
  
  // Moving if average movement > threshold OR any single jump is large (> 50px)
  return avg_movement > threshold || max_single_move > 50.0;
}


// HYBRID DETECTION: Three-phase smart detection with movement analysis
// Phase 1: Coarse scan (user's sample_interval) to find all logo segments with positions
// Phase 2: Classify segments as MOVING or STATIC based on position changes
// Phase 3: Fine-scan (interval=1) ONLY for MOVING segments — the key optimization
std::vector<DetectionResult> detect_logos_hybrid(
    const std::string& video_path,
    fg::VideoLayoutManager& layout_manager,
    const fg::VideoLayout& layout,
    int coarse_interval,
    bool use_multi_scale)
{
  std::vector<DetectionResult> results;
  auto hybrid_start = std::chrono::steady_clock::now();
  double setup_time = 0.0;
  double phase1_time = 0.0;
  double phase2_time = 0.0;
  double phase3_time = 0.0;
  double phase35_time = 0.0;
  double final_build_time = 0.0;
  long long p1_seek_us = 0, p1_read_us = 0, p3_seek_us = 0, p3_read_us = 0;
  
  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) {
    std::cerr << "  Error: Cannot open video for detection" << std::endl;
    return results;
  }
  
  int frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  int frame_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int frame_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  double fps = cap.get(cv::CAP_PROP_FPS);
  if (fps <= 0) fps = 30.0;
  
  std::cout << "\n  ========================================================" << std::endl;
  std::cout << "  [HYBRID MODE] Smart interval-based detection" << std::endl;
  std::cout << "  Video: " << frame_count << " frames, " << fps << " FPS, "
            << std::fixed << std::setprecision(1) << (frame_count / fps) << "s total" << std::endl;
  std::cout << "  Coarse interval: " << coarse_interval << " frames" << std::endl;
  std::cout << "  ========================================================" << std::endl;
  
  // Build per-detection state
  std::vector<DetectionState> states;
  for (const auto& det : layout.detections) {
    std::string ref_path = layout_manager.get_reference_path(det.reference_image);
    cv::Mat template_img = cv::imread(ref_path);
    if (template_img.empty()) {
      std::cerr << "  Error: Cannot load reference image: " << ref_path << std::endl;
      continue;
    }
    fg::SearchRegion effective_region;
    if (det.search_quadrant >= 1 && det.search_quadrant <= 4) {
      effective_region = fg::search_region_from_quadrant(frame_width, frame_height, det.search_quadrant);
    } else if (det.search_region.width > 0 && det.search_region.height > 0) {
      effective_region = det.search_region;
    } else {
      effective_region = fg::SearchRegion(0, 0, frame_width, frame_height);
    }
    DetectionState ds;
    ds.template_img = template_img;
    ds.effective_region = effective_region;
    ds.match_threshold = det.match_threshold;
    ds.replacement_image = det.replacement_image;
    ds.scale = det.replacement_scale;
    ds.name = det.name;
    ds.full_screen = det.full_screen;
    ds.suppress_during_full_screen = det.suppress_during_full_screen;
    states.push_back(ds);
    std::cout << "  Tracking: " << det.name << " (threshold=" << det.match_threshold << ")" << std::endl;
  }
  setup_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - hybrid_start).count();
  std::vector<long long> p1_detect_us(states.size(), 0), p3_detect_us(states.size(), 0);
  std::vector<int> p1_detect_calls(states.size(), 0), p3_detect_calls(states.size(), 0);
  
  // ================================================================
  // PHASE 1: Coarse scan with position tracking
  // ================================================================
  std::cout << "\n  --- PHASE 1: Coarse scan (interval=" << coarse_interval << " frames) ---" << std::endl;
  auto phase1_start = std::chrono::steady_clock::now();
  
  const int MAX_CONSECUTIVE_MISSES = 5;
  
  // Per-detection coarse positions: vector of (frame_num, (x, y))
  std::vector<std::vector<std::pair<int, std::pair<int, int>>>> coarse_positions(states.size());
  
  int frames_scanned_p1 = 0;
  for (int frame_num = 0; frame_num < frame_count; frame_num += coarse_interval) {
    auto seek_t0 = std::chrono::steady_clock::now();
    cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);
    p1_seek_us += std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - seek_t0).count();
    cv::Mat frame;
    auto read_t0 = std::chrono::steady_clock::now();
    if (!cap.read(frame)) break;
    p1_read_us += std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - read_t0).count();
    frames_scanned_p1++;
    
    for (size_t idx = 0; idx < states.size(); ++idx) {
      DetectionState& ds = states[idx];
      int found_x, found_y;
      double confidence;
      auto detect_t0 = std::chrono::steady_clock::now();
      bool found = detect_logo_in_frame(frame, ds.template_img, ds.effective_region,
                                        ds.match_threshold, found_x, found_y, confidence, use_multi_scale);
      p1_detect_us[idx] += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - detect_t0).count();
      p1_detect_calls[idx]++;
      if (found) {
        if (!ds.currently_detecting) {
          ds.currently_detecting = true;
          ds.detection_start_frame = frame_num;
          std::cout << "    [" << ds.name << "] DETECTED at frame " << frame_num
                    << " (t=" << std::fixed << std::setprecision(1) << (frame_num / fps) << "s)"
                    << " pos=(" << found_x << "," << found_y << ")"
                    << " conf=" << std::fixed << std::setprecision(0) << (confidence * 100) << "%" << std::endl;
        }
        coarse_positions[idx].push_back({frame_num, {found_x, found_y}});
        ds.last_detected_x = found_x;
        ds.last_detected_y = found_y;
        ds.last_found_frame = frame_num;
        ds.consecutive_misses = 0;
      } else {
        if (ds.currently_detecting) {
          ds.consecutive_misses++;
          if (ds.consecutive_misses >= MAX_CONSECUTIVE_MISSES) {
            std::cout << "    [" << ds.name << "] LOST at frame " << ds.last_found_frame
                      << " (t=" << std::fixed << std::setprecision(1) << (ds.last_found_frame / fps) << "s)"
                      << " | segment=" << (ds.last_found_frame - ds.detection_start_frame) << " frames" << std::endl;
            ds.currently_detecting = false;
            ds.consecutive_misses = 0;
          }
        }
      }
    }
    
    if (frame_num % 300 == 0) {
      std::cout << "    Scanning: " << frame_num << "/" << frame_count
                << " (" << (frame_num * 100 / std::max(1, frame_count)) << "%)\r" << std::flush;
    }
  }
  
  phase1_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase1_start).count();
  std::cout << "\n  Phase 1 complete: scanned " << frames_scanned_p1 << " frames in "
            << std::fixed << std::setprecision(1) << phase1_time << "s" << std::endl;
  
  // ================================================================
  // PHASE 2: Build segments and classify MOVING vs STATIC
  // ================================================================
  auto phase2_start = std::chrono::steady_clock::now();
  std::cout << "\n  --- PHASE 2: Classify segments (MOVING vs STATIC) ---" << std::endl;
  
  struct CoarseSegment {
    size_t det_idx;
    int start_frame;
    int end_frame;
    std::vector<std::pair<int, std::pair<int, int>>> positions;
    bool is_moving;
  };
  std::vector<CoarseSegment> segments;
  
  for (size_t idx = 0; idx < states.size(); ++idx) {
    const auto& positions = coarse_positions[idx];
    if (positions.empty()) continue;
    
    int gap_threshold = MAX_CONSECUTIVE_MISSES * coarse_interval;

    // First split by large temporal gaps (logo truly absent for a while).
    std::vector<std::vector<std::pair<int, std::pair<int, int>>>> base_segments;
    std::vector<std::pair<int, std::pair<int, int>>> current;
    current.push_back(positions[0]);
    for (size_t i = 1; i < positions.size(); ++i) {
      int gap = positions[i].first - positions[i-1].first;
      if (gap > gap_threshold) {
        if (!current.empty()) base_segments.push_back(current);
        current.clear();
      }
      current.push_back(positions[i]);
    }
    if (!current.empty()) base_segments.push_back(current);

    // Then split each base segment into moving/static runs by per-step displacement.
    // This prevents "entire video moving" when only a short interval actually moves.
    const double motion_step_threshold = 12.0;  // px between coarse samples
    for (const auto& base : base_segments) {
      if (base.empty()) continue;

      if (base.size() < 2) {
        CoarseSegment only;
        only.det_idx = idx;
        only.start_frame = base.front().first;
        only.end_frame = base.back().first;
        only.positions = base;
        only.is_moving = false;
        segments.push_back(only);
        continue;
      }

      std::vector<bool> moving_point(base.size(), false);
      for (size_t i = 1; i < base.size(); ++i) {
        int dx = base[i].second.first - base[i-1].second.first;
        int dy = base[i].second.second - base[i-1].second.second;
        double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
        if (dist >= motion_step_threshold) {
          moving_point[i-1] = true;
          moving_point[i] = true;
        }
      }

      // Dilate by one coarse sample to include transition frames around motion.
      std::vector<bool> expanded = moving_point;
      for (size_t i = 0; i < moving_point.size(); ++i) {
        if (!moving_point[i]) continue;
        if (i > 0) expanded[i-1] = true;
        if (i + 1 < moving_point.size()) expanded[i+1] = true;
      }
      moving_point.swap(expanded);

      size_t run_start = 0;
      while (run_start < base.size()) {
        bool run_moving = moving_point[run_start];
        size_t run_end = run_start + 1;
        while (run_end < base.size() && moving_point[run_end] == run_moving) {
          run_end++;
        }

        CoarseSegment out;
        out.det_idx = idx;
        out.positions.assign(base.begin() + run_start, base.begin() + run_end);
        out.start_frame = out.positions.front().first;
        out.end_frame = out.positions.back().first;
        out.is_moving = run_moving;

        // Single-point "moving" runs are usually noise; treat as static.
        if (out.is_moving && out.positions.size() < 2) {
          out.is_moving = false;
        }

        // If a STATIC segment with multiple coarse samples reaches near boundaries,
        // extend it to avoid static logo flashing at start/end.
        if (!out.is_moving && out.positions.size() >= 2) {
          if (out.end_frame >= frame_count - coarse_interval - 1) {
            out.end_frame = frame_count - 1;
          }
          if (out.start_frame <= coarse_interval) {
            out.start_frame = 0;
          }
        }

        segments.push_back(out);
        run_start = run_end;
      }
    }
  }
  
  int moving_count = 0, static_count = 0;
  double total_moving_sec = 0, total_static_sec = 0;
  
  for (const auto& seg : segments) {
    const DetectionState& ds = states[seg.det_idx];
    double start_sec = seg.start_frame / fps;
    double end_sec = seg.end_frame / fps;
    double duration = end_sec - start_sec;
    
    if (seg.is_moving) {
      moving_count++;
      total_moving_sec += duration;
      int min_x = seg.positions[0].second.first, max_x = min_x;
      int min_y = seg.positions[0].second.second, max_y = min_y;
      for (const auto& p : seg.positions) {
        min_x = std::min(min_x, p.second.first);
        max_x = std::max(max_x, p.second.first);
        min_y = std::min(min_y, p.second.second);
        max_y = std::max(max_y, p.second.second);
      }
      std::cout << "    [" << ds.name << "] >> MOVING  frames " << seg.start_frame << "-" << seg.end_frame
                << " (" << std::fixed << std::setprecision(1) << start_sec << "s - " << end_sec << "s"
                << ", dur=" << duration << "s)"
                << " x:[" << min_x << "-" << max_x << "] y:[" << min_y << "-" << max_y << "]"
                << " (" << seg.positions.size() << " coarse samples)" << std::endl;
    } else {
      static_count++;
      total_static_sec += duration;
      int mid = seg.positions.size() / 2;
      std::cout << "    [" << ds.name << "]    STATIC  frames " << seg.start_frame << "-" << seg.end_frame
                << " (" << std::fixed << std::setprecision(1) << start_sec << "s - " << end_sec << "s"
                << ", dur=" << duration << "s)"
                << " pos=(" << seg.positions[mid].second.first << "," << seg.positions[mid].second.second << ")"
                << " (" << seg.positions.size() << " coarse samples)" << std::endl;
    }
  }
  
  std::cout << "  ------------------------------------------------" << std::endl;
  std::cout << "  Total: " << segments.size() << " segments = "
            << moving_count << " MOVING (" << std::fixed << std::setprecision(1) << total_moving_sec << "s) + "
            << static_count << " STATIC (" << total_static_sec << "s)" << std::endl;
  
  // Log a clear summary of moving logo intervals
  if (moving_count > 0) {
    std::cout << "\n  ****************************************************" << std::endl;
    std::cout << "  *          MOVING LOGO INTERVALS DETECTED          *" << std::endl;
    std::cout << "  ****************************************************" << std::endl;
    int interval_num = 1;
    for (const auto& seg : segments) {
      if (!seg.is_moving) continue;
      const DetectionState& ds = states[seg.det_idx];
      double start_sec = seg.start_frame / fps;
      double end_sec = seg.end_frame / fps;
      int start_min = static_cast<int>(start_sec) / 60;
      int start_s   = static_cast<int>(start_sec) % 60;
      int end_min   = static_cast<int>(end_sec) / 60;
      int end_s     = static_cast<int>(end_sec) % 60;
      std::cout << "  *  Interval " << interval_num++ << ": [" << ds.name << "]" << std::endl;
      std::cout << "  *    Time:   " << std::setfill('0') << std::setw(2) << start_min << ":"
                << std::setw(2) << start_s << " - "
                << std::setw(2) << end_min << ":" << std::setw(2) << end_s << std::setfill(' ')
                << "  (" << std::fixed << std::setprecision(1) << (end_sec - start_sec) << "s)" << std::endl;
      std::cout << "  *    Frames: " << seg.start_frame << " - " << seg.end_frame << std::endl;
      // Position movement range
      int min_x = seg.positions[0].second.first, max_x = min_x;
      int min_y = seg.positions[0].second.second, max_y = min_y;
      for (const auto& p : seg.positions) {
        min_x = std::min(min_x, p.second.first);
        max_x = std::max(max_x, p.second.first);
        min_y = std::min(min_y, p.second.second);
        max_y = std::max(max_y, p.second.second);
      }
      std::cout << "  *    Move:   x=" << min_x << " -> " << max_x
                << "  y=" << min_y << " -> " << max_y << std::endl;
    }
    std::cout << "  *" << std::endl;
    std::cout << "  *  These intervals will be fine-scanned at interval=1" << std::endl;
    std::cout << "  *  All other intervals use fast FFmpeg processing" << std::endl;
    std::cout << "  ****************************************************" << std::endl;
  }
  phase2_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase2_start).count();

  // ================================================================
  // PHASE 3: Fine scan (interval=1) for MOVING segments ONLY
  // ================================================================
  int total_fine_frames = 0;
  if (moving_count > 0) {
    std::cout << "\n  --- PHASE 3: Fine scan (interval=1) for " << moving_count << " MOVING segment(s) ---" << std::endl;
    auto phase3_start = std::chrono::steady_clock::now();
    
    const int edge_guard_px = 8;           // Expand visibility tests near edges
    const int exit_tail_guard_frames = 8;  // Continue extrapolation a few frames after expected exit
    for (auto& seg : segments) {
      if (!seg.is_moving) continue;
      
      const DetectionState& ds = states[seg.det_idx];
      // Pad scan range by coarse_interval to catch boundary frames
      int range_start = std::max(0, seg.start_frame - coarse_interval);
      int range_end = std::min(frame_count - 1, seg.end_frame + coarse_interval);
      int range_frames = range_end - range_start + 1;
      total_fine_frames += range_frames;
      
      std::cout << "    [" << ds.name << "] Fine-scanning frames " << range_start << " to " << range_end
                << " (" << range_frames << " frames, "
                << std::fixed << std::setprecision(1) << (range_frames / fps) << "s)..." << std::endl;
      
      // Replace coarse positions with fine per-frame positions
      seg.positions.clear();
      int found_count = 0;
      
      for (int f = range_start; f <= range_end; ++f) {
        auto seek_t0 = std::chrono::steady_clock::now();
        cap.set(cv::CAP_PROP_POS_FRAMES, f);
        p3_seek_us += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - seek_t0).count();
        cv::Mat frame;
        auto read_t0 = std::chrono::steady_clock::now();
        if (!cap.read(frame)) break;
        p3_read_us += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - read_t0).count();
        
        int found_x, found_y;
        double confidence;
        auto detect_t0 = std::chrono::steady_clock::now();
        bool found = detect_logo_in_frame(frame, ds.template_img, ds.effective_region,
                                          ds.match_threshold, found_x, found_y, confidence, use_multi_scale);
        p3_detect_us[seg.det_idx] += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - detect_t0).count();
        p3_detect_calls[seg.det_idx]++;
        if (found) {
          seg.positions.push_back({f, {found_x, found_y}});
          found_count++;
        }
        
        if ((f - range_start) % 100 == 0 && f > range_start) {
          int pct = (f - range_start) * 100 / std::max(1, range_frames);
          std::cout << "      Progress: " << (f - range_start) << "/" << range_frames
                    << " (" << pct << "%) found=" << found_count << "\r" << std::flush;
        }
      }
      std::cout << std::endl;
      
      // Update segment boundaries from fine scan results
      if (!seg.positions.empty()) {
        // Keep only the truly moving core inside the scanned range.
        // Without this, the +/-coarse padding can pull static frames into a MOVING segment.
        if (seg.positions.size() >= 3) {
          const double per_frame_motion_threshold = 0.8;  // px/frame
          const int transition_pad_frames = 8;            // keep a small context around movement
          std::vector<bool> moving_point(seg.positions.size(), false);

          for (size_t i = 1; i < seg.positions.size(); ++i) {
            int dx = seg.positions[i].second.first - seg.positions[i-1].second.first;
            int dy = seg.positions[i].second.second - seg.positions[i-1].second.second;
            double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            if (dist >= per_frame_motion_threshold) {
              moving_point[i-1] = true;
              moving_point[i] = true;
            }
          }

          int first_move = -1, last_move = -1;
          for (size_t i = 0; i < moving_point.size(); ++i) {
            if (moving_point[i]) {
              if (first_move < 0) first_move = static_cast<int>(i);
              last_move = static_cast<int>(i);
            }
          }

          if (first_move >= 0 && last_move >= first_move) {
            int keep_start = std::max(0, first_move - transition_pad_frames);
            int keep_end = std::min(static_cast<int>(seg.positions.size()) - 1, last_move + transition_pad_frames);
            seg.positions = std::vector<std::pair<int, std::pair<int, int>>>(
              seg.positions.begin() + keep_start, seg.positions.begin() + keep_end + 1);
          }
        }

        seg.start_frame = seg.positions.front().first;
        seg.end_frame = seg.positions.back().first;
        std::cout << "    [" << ds.name << "] Fine result: " << found_count << "/" << range_frames
                  << " frames with logo, moving-core range=" << seg.start_frame << "-" << seg.end_frame << std::endl;
      } else {
        std::cout << "    [" << ds.name << "] Fine result: NO positions found (segment will be skipped)" << std::endl;
      }
    }
    
    phase3_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase3_start).count();
    std::cout << "  Phase 3 complete: " << total_fine_frames << " frames fine-scanned in "
              << std::fixed << std::setprecision(1) << phase3_time << "s" << std::endl;
    
    // ── PHASE 3.5: Extrapolate positions at boundaries for partially-visible logos ──
    // When a logo slides in or out of frame, template matching fails for partially-visible frames.
    // We estimate where the logo would be using velocity from the last/first few detected positions.
    auto phase35_start = std::chrono::steady_clock::now();
    std::cout << "\n  --- PHASE 3.5: Extrapolating boundary positions ---" << std::endl;
    for (auto& seg : segments) {
      if (!seg.is_moving || seg.positions.size() < 2) continue;
      
      const DetectionState& ds = states[seg.det_idx];
      int logo_w = static_cast<int>(ds.template_img.cols * ds.scale);
      int logo_h = static_cast<int>(ds.template_img.rows * ds.scale);
      int positions_before = static_cast<int>(seg.positions.size());
      
      // --- Backward extrapolation (logo entering the frame) ---
      {
        int n = std::min(static_cast<int>(seg.positions.size()), 5);
        double vx = 0, vy = 0;
        int count = 0;
        for (int i = 1; i < n; ++i) {
          int df = seg.positions[i].first - seg.positions[i-1].first;
          if (df > 0) {
            vx += static_cast<double>(seg.positions[i].second.first - seg.positions[i-1].second.first) / df;
            vy += static_cast<double>(seg.positions[i].second.second - seg.positions[i-1].second.second) / df;
            count++;
          }
        }
        if (count > 0 && (std::abs(vx) > 0.5 || std::abs(vy) > 0.5)) {
          vx /= count;
          vy /= count;
          int first_frame = seg.positions.front().first;
          int first_x = seg.positions.front().second.first;
          int first_y = seg.positions.front().second.second;
          
          // How many frames until logo is fully off-screen (going backward)
          double speed = std::max(std::abs(vx), std::abs(vy));
          int max_extra = static_cast<int>(std::ceil((std::max(logo_w, logo_h) + 2 * edge_guard_px) / speed)) + exit_tail_guard_frames;
          max_extra = std::min(max_extra, 90);  // Cap at 3 seconds
          
          std::vector<std::pair<int, std::pair<int, int>>> extra;
          for (int i = 1; i <= max_extra; ++i) {
            int f = first_frame - i;
            if (f < 0) break;
            int ex = first_x - static_cast<int>(std::round(vx * i));
            int ey = first_y - static_cast<int>(std::round(vy * i));
            // Only add if logo would be at least partially visible
            if (ex + logo_w + edge_guard_px > 0 && ex - edge_guard_px < frame_width &&
                ey + logo_h + edge_guard_px > 0 && ey - edge_guard_px < frame_height) {
              extra.push_back({f, {ex, ey}});
            } else {
              break;  // Logo fully off-screen, stop
            }
          }
          if (!extra.empty()) {
            std::reverse(extra.begin(), extra.end());
            seg.positions.insert(seg.positions.begin(), extra.begin(), extra.end());
          }
        }
      }
      
      // --- Forward extrapolation (logo exiting the frame) ---
      {
        int n = std::min(static_cast<int>(seg.positions.size()), 5);
        int start_idx = static_cast<int>(seg.positions.size()) - n;
        double vx = 0, vy = 0;
        int count = 0;
        for (int i = start_idx + 1; i < static_cast<int>(seg.positions.size()); ++i) {
          int df = seg.positions[i].first - seg.positions[i-1].first;
          if (df > 0) {
            vx += static_cast<double>(seg.positions[i].second.first - seg.positions[i-1].second.first) / df;
            vy += static_cast<double>(seg.positions[i].second.second - seg.positions[i-1].second.second) / df;
            count++;
          }
        }
        if (count > 0 && (std::abs(vx) > 0.5 || std::abs(vy) > 0.5)) {
          vx /= count;
          vy /= count;
          int last_frame = seg.positions.back().first;
          int last_x = seg.positions.back().second.first;
          int last_y = seg.positions.back().second.second;
          
          double speed = std::max(std::abs(vx), std::abs(vy));
          int max_extra = static_cast<int>(std::ceil((std::max(logo_w, logo_h) + 2 * edge_guard_px) / speed)) + exit_tail_guard_frames;
          max_extra = std::min(max_extra, 90);
          
          for (int i = 1; i <= max_extra; ++i) {
            int f = last_frame + i;
            if (f >= frame_count) break;
            int ex = last_x + static_cast<int>(std::round(vx * i));
            int ey = last_y + static_cast<int>(std::round(vy * i));
            if (ex + logo_w + edge_guard_px > 0 && ex - edge_guard_px < frame_width &&
                ey + logo_h + edge_guard_px > 0 && ey - edge_guard_px < frame_height) {
              seg.positions.push_back({f, {ex, ey}});
            } else {
              break;
            }
          }
        }
      }
      
      // --- Internal gap extrapolation (for mid-segment exit/re-entry) ---
      // When a logo exits and re-enters within the same segment, template matching
      // fails in the gap. Fill these gaps with extrapolated exit/re-entry trajectories.
      {
        std::vector<std::pair<int, std::pair<int, int>>> gap_extras;
        
        for (size_t gi = 1; gi < seg.positions.size(); ++gi) {
          int gap = seg.positions[gi].first - seg.positions[gi-1].first;
          if (gap <= 3) continue;  // Small gap — runtime interpolation handles it fine
          
          int prev_x = seg.positions[gi-1].second.first;
          int prev_y = seg.positions[gi-1].second.second;
          int next_x = seg.positions[gi].second.first;
          int next_y = seg.positions[gi].second.second;
          
          // Compute velocity BEFORE the gap (exit velocity, from up to 5 preceding pairs)
          double vx_exit = 0, vy_exit = 0;
          int cnt_exit = 0;
          for (int j = static_cast<int>(gi) - 1; j >= 1 && cnt_exit < 5; --j) {
            int df = seg.positions[j].first - seg.positions[j-1].first;
            if (df > 0 && df <= 5) {
              vx_exit += static_cast<double>(seg.positions[j].second.first - seg.positions[j-1].second.first) / df;
              vy_exit += static_cast<double>(seg.positions[j].second.second - seg.positions[j-1].second.second) / df;
              cnt_exit++;
            } else if (df > 5) {
              break;
            }
          }
          if (cnt_exit > 0) { vx_exit /= cnt_exit; vy_exit /= cnt_exit; }
          
          // Check if this gap is a discontinuity (exit + re-entry, not just a detection miss)
          double expected_x = prev_x + vx_exit * gap;
          double expected_y = prev_y + vy_exit * gap;
          double deviation = std::sqrt((next_x - expected_x) * (next_x - expected_x) +
                                       (next_y - expected_y) * (next_y - expected_y));
          double logo_dim = std::max(logo_w, logo_h);
          if (deviation < logo_dim * 0.5) continue;  // Smooth — runtime interpolation is fine
          
          // This IS a discontinuity — fill exit and re-entry trajectories
          int gap_start = seg.positions[gi-1].first;
          int gap_end = seg.positions[gi].first;
          int mid_frame = (gap_start + gap_end) / 2;
          
          // Forward extrapolation from exit (first half of gap)
          if (cnt_exit > 0 && (std::abs(vx_exit) > 0.5 || std::abs(vy_exit) > 0.5)) {
            for (int step = 1; gap_start + step <= mid_frame; ++step) {
              int f = gap_start + step;
              int ex = prev_x + static_cast<int>(std::round(vx_exit * step));
              int ey = prev_y + static_cast<int>(std::round(vy_exit * step));
              if (ex + logo_w + edge_guard_px > 0 && ex - edge_guard_px < frame_width &&
                  ey + logo_h + edge_guard_px > 0 && ey - edge_guard_px < frame_height) {
                gap_extras.push_back({f, {ex, ey}});
              } else {
                break;  // Logo fully off-screen
              }
            }
          }
          
          // Compute velocity AFTER the gap (re-entry velocity, from up to 5 following pairs)
          double vx_entry = 0, vy_entry = 0;
          int cnt_entry = 0;
          for (size_t j = gi; j < seg.positions.size() - 1 && cnt_entry < 5; ++j) {
            int df = seg.positions[j+1].first - seg.positions[j].first;
            if (df > 0 && df <= 5) {
              vx_entry += static_cast<double>(seg.positions[j+1].second.first - seg.positions[j].second.first) / df;
              vy_entry += static_cast<double>(seg.positions[j+1].second.second - seg.positions[j].second.second) / df;
              cnt_entry++;
            } else if (df > 5) {
              break;
            }
          }
          if (cnt_entry > 0) { vx_entry /= cnt_entry; vy_entry /= cnt_entry; }
          
          // Backward extrapolation from re-entry (second half of gap)
          if (cnt_entry > 0 && (std::abs(vx_entry) > 0.5 || std::abs(vy_entry) > 0.5)) {
            for (int step = 1; gap_end - step > mid_frame; ++step) {
              int f = gap_end - step;
              int ex = next_x - static_cast<int>(std::round(vx_entry * step));
              int ey = next_y - static_cast<int>(std::round(vy_entry * step));
              if (ex + logo_w + edge_guard_px > 0 && ex - edge_guard_px < frame_width &&
                  ey + logo_h + edge_guard_px > 0 && ey - edge_guard_px < frame_height) {
                gap_extras.push_back({f, {ex, ey}});
              } else {
                break;
              }
            }
          }
        }
        
        if (!gap_extras.empty()) {
          seg.positions.insert(seg.positions.end(), gap_extras.begin(), gap_extras.end());
          std::sort(seg.positions.begin(), seg.positions.end());
          // Remove duplicate frames (keep first = exit trajectory for early frames)
          auto dedup_end = std::unique(seg.positions.begin(), seg.positions.end(),
                                       [](const auto& a, const auto& b) { return a.first == b.first; });
          seg.positions.erase(dedup_end, seg.positions.end());
          std::cout << "    [" << ds.name << "] Filled " << gap_extras.size()
                    << " internal gap frames (discontinuities)" << std::endl;
        }
      }
      
      // Update boundaries after extrapolation
      if (!seg.positions.empty()) {
        seg.start_frame = seg.positions.front().first;
        seg.end_frame = seg.positions.back().first;
      }
      
      int added = static_cast<int>(seg.positions.size()) - positions_before;
      if (added > 0) {
        std::cout << "    [" << ds.name << "] Extrapolated " << added << " total extra frames"
                  << " (new range: " << seg.start_frame << "-" << seg.end_frame << ")" << std::endl;
      }
    }

    // Close inter-segment gaps for the same detection to avoid uncovered frames
    // where the old logo can briefly appear. Prefer assigning gaps to MOVING ranges,
    // because moving segments can extrapolate positions for those boundary frames.
    for (size_t det_idx = 0; det_idx < states.size(); ++det_idx) {
      std::vector<size_t> idxs;
      for (size_t si = 0; si < segments.size(); ++si) {
        if (segments[si].det_idx == det_idx) idxs.push_back(si);
      }
      if (idxs.size() < 2) continue;

      std::sort(idxs.begin(), idxs.end(),
                [&](size_t a, size_t b) { return segments[a].start_frame < segments[b].start_frame; });

      for (size_t k = 1; k < idxs.size(); ++k) {
        auto& left = segments[idxs[k - 1]];
        auto& right = segments[idxs[k]];
        if (right.start_frame <= left.end_frame + 1) continue;  // no gap

        int old_left_end = left.end_frame;
        int old_right_start = right.start_frame;

        if (!left.is_moving && right.is_moving) {
          // static -> moving: pull moving earlier to cover the gap
          right.start_frame = left.end_frame + 1;
        } else if (left.is_moving && !right.is_moving) {
          // moving -> static: extend moving later to cover the gap
          left.end_frame = right.start_frame - 1;
        } else if (!left.is_moving && !right.is_moving) {
          // static -> static: split the gap at midpoint
          int mid = (left.end_frame + right.start_frame) / 2;
          left.end_frame = mid;
          right.start_frame = mid + 1;
        } else {
          // moving -> moving: split the gap at midpoint
          int mid = (left.end_frame + right.start_frame) / 2;
          left.end_frame = mid;
          right.start_frame = mid + 1;
        }

        if (left.end_frame != old_left_end || right.start_frame != old_right_start) {
          std::cout << "    [" << states[det_idx].name << "] Closed gap by boundary adjust: "
                    << old_left_end << ".." << old_right_start
                    << " -> " << left.end_frame << ".." << right.start_frame << std::endl;
        }
      }
    }
    phase35_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase35_start).count();
    
    // Performance comparison
    int hybrid_total = frames_scanned_p1 + total_fine_frames;
    int full_scan = frame_count;
    int saved = full_scan - hybrid_total;
    int pct_of_full = hybrid_total * 100 / std::max(1, full_scan);
    std::cout << "\n  +-- PERFORMANCE COMPARISON ---------------------------+" << std::endl;
    std::cout << "  | Full interval-1 scan would be: " << std::setw(7) << full_scan << " frames    |" << std::endl;
    std::cout << "  | Hybrid scan total:             " << std::setw(7) << hybrid_total << " frames    |" << std::endl;
    std::cout << "  | Frames saved:                  " << std::setw(7) << saved << " (" << pct_of_full << "% of full) |" << std::endl;
    std::cout << "  +----------------------------------------------------+" << std::endl;
  } else {
    std::cout << "\n  --- PHASE 3: SKIPPED (no moving segments found) ---" << std::endl;
    std::cout << "  All segments are STATIC -> will use fast FFmpeg processing" << std::endl;
    phase3_time = 0.0;
    phase35_time = 0.0;
  }
  
  // ================================================================
  // Build final DetectionResult list
  // ================================================================
  auto final_build_start = std::chrono::steady_clock::now();
  std::cout << "\n  --- Final Detection Results ---" << std::endl;
  
  for (const auto& seg : segments) {
    if (seg.positions.empty()) continue;
    
    const DetectionState& ds = states[seg.det_idx];
    
    DetectionResult result;
    result.found = true;
    result.is_moving = seg.is_moving;
    result.name = ds.name;
    result.width = ds.template_img.cols;
    result.height = ds.template_img.rows;
    result.replacement_image = ds.replacement_image;
    result.scale = ds.scale;
    result.full_screen = ds.full_screen;
    result.suppress_during_full_screen = ds.suppress_during_full_screen;
    result.start_frame = seg.start_frame;
    result.end_frame = seg.end_frame;
    result.confidence = 0.0;
    
    if (seg.is_moving) {
      result.tracked_positions = seg.positions;
      result.x = seg.positions[0].second.first;
      result.y = seg.positions[0].second.second;
    } else {
      // Use median position for static segments
      int mid = seg.positions.size() / 2;
      result.x = seg.positions[mid].second.first;
      result.y = seg.positions[mid].second.second;
    }
    
    results.push_back(result);
    
    std::string type_label = result.is_moving ? ">> MOVING" : "   STATIC";
    std::cout << "    " << type_label << " [" << result.name << "]"
              << " frames " << result.start_frame << "-" << result.end_frame
              << " pos=(" << result.x << "," << result.y << ")";
    if (result.is_moving) {
      std::cout << " tracked=" << result.tracked_positions.size() << " positions";
    }
    std::cout << std::endl;
  }
  
  std::cout << "  ========================================================" << std::endl;
  final_build_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - final_build_start).count();

  // Comprehensive detection latency breakdown
  double p1_seek_s = p1_seek_us / 1e6;
  double p1_read_s = p1_read_us / 1e6;
  double p3_seek_s = p3_seek_us / 1e6;
  double p3_read_s = p3_read_us / 1e6;
  double p1_detect_s = 0.0, p3_detect_s = 0.0;
  int p1_calls_total = 0, p3_calls_total = 0;
  for (size_t i = 0; i < states.size(); ++i) {
    p1_detect_s += p1_detect_us[i] / 1e6;
    p3_detect_s += p3_detect_us[i] / 1e6;
    p1_calls_total += p1_detect_calls[i];
    p3_calls_total += p3_detect_calls[i];
  }
  double hybrid_total = std::chrono::duration<double>(std::chrono::steady_clock::now() - hybrid_start).count();
  double accounted = setup_time + phase1_time + phase2_time + phase3_time + phase35_time + final_build_time;
  double unaccounted = std::max(0.0, hybrid_total - accounted);

  std::cout << "\n  --- Detection Latency Breakdown (HYBRID) ---" << std::endl;
  std::cout << "    Setup/templates:      " << std::fixed << std::setprecision(3) << setup_time << "s" << std::endl;
  std::cout << "    Phase 1 total:        " << phase1_time << "s" << std::endl;
  std::cout << "      - seek:             " << p1_seek_s << "s" << std::endl;
  std::cout << "      - read:             " << p1_read_s << "s" << std::endl;
  std::cout << "      - detect calls:     " << p1_detect_s << "s"
            << " (" << p1_calls_total << " calls, "
            << ((p1_calls_total > 0) ? ((p1_detect_s * 1000.0) / p1_calls_total) : 0.0) << " ms/call)" << std::endl;
  std::cout << "    Phase 2 total:        " << phase2_time << "s" << std::endl;
  std::cout << "    Phase 3 total:        " << phase3_time << "s" << std::endl;
  std::cout << "      - seek:             " << p3_seek_s << "s" << std::endl;
  std::cout << "      - read:             " << p3_read_s << "s" << std::endl;
  std::cout << "      - detect calls:     " << p3_detect_s << "s"
            << " (" << p3_calls_total << " calls, "
            << ((p3_calls_total > 0) ? ((p3_detect_s * 1000.0) / p3_calls_total) : 0.0) << " ms/call)" << std::endl;
  std::cout << "    Phase 3.5 total:      " << phase35_time << "s" << std::endl;
  std::cout << "    Final result build:   " << final_build_time << "s" << std::endl;
  std::cout << "    -------------------------------------------" << std::endl;
  std::cout << "    HYBRID detection total: " << hybrid_total << "s" << std::endl;
  std::cout << "    Accounted subtotal:     " << accounted << "s" << std::endl;
  std::cout << "    Unaccounted:            " << unaccounted << "s" << std::endl;

  std::cout << "    Per-logo detection calls:" << std::endl;
  for (size_t i = 0; i < states.size(); ++i) {
    double p1s = p1_detect_us[i] / 1e6;
    double p3s = p3_detect_us[i] / 1e6;
    int c1 = p1_detect_calls[i];
    int c3 = p3_detect_calls[i];
    std::cout << "      [" << states[i].name << "] P1: " << c1 << " calls, " << p1s << "s"
              << " | P3: " << c3 << " calls, " << p3s << "s" << std::endl;
  }
  
  cap.release();
  return results;
}


// Detect logo presence: one pass over video, all detections per frame (optimized)
 std::vector<DetectionResult> detect_logos_in_video(
     const std::string& video_path,
     fg::VideoLayoutManager& layout_manager,
     const fg::VideoLayout& layout,
     int sample_interval,
     bool use_multi_scale)
 {
   std::vector<DetectionResult> results;
   
   cv::VideoCapture cap(video_path);
   if (!cap.isOpened()) {
     std::cerr << "  Error: Cannot open video for detection" << std::endl;
     return results;
   }
   
   int frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
   int frame_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
   int frame_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
   
   // Build per-detection state (templates + regions); skip failed loads
   std::vector<DetectionState> states;
   for (const auto& det : layout.detections) {
     std::string ref_path = layout_manager.get_reference_path(det.reference_image);
     cv::Mat template_img = cv::imread(ref_path);
     if (template_img.empty()) {
       std::cerr << "  Error: Cannot load reference image: " << ref_path << std::endl;
       continue;
     }
     fg::SearchRegion effective_region;
     if (det.search_quadrant >= 1 && det.search_quadrant <= 4) {
       effective_region = fg::search_region_from_quadrant(frame_width, frame_height, det.search_quadrant);
     } else if (det.search_region.width > 0 && det.search_region.height > 0) {
       effective_region = det.search_region;
     } else {
       effective_region = fg::SearchRegion(0, 0, frame_width, frame_height);
     }
     DetectionState ds;
     ds.template_img = template_img;
     ds.effective_region = effective_region;
     ds.match_threshold = det.match_threshold;
     ds.replacement_image = det.replacement_image;
     ds.scale = det.replacement_scale;
     ds.name = det.name;
     ds.full_screen = det.full_screen;
     ds.suppress_during_full_screen = det.suppress_during_full_screen;
     states.push_back(ds);
     std::cout << "  Detecting: " << det.name
               << " region (" << effective_region.x << "," << effective_region.y << ") "
               << effective_region.width << "x" << effective_region.height << std::endl;
   }
   
   const int MAX_CONSECUTIVE_MISSES = 5;
   const int START_PADDING_FRAMES = 1;     // Start overlay earlier so new logo is on screen before old one appears
   const int END_PADDING_FRAMES = 1;       // Extra frames at segment end only (reduces original-logo flash)
   const int BOUNDARY_REFINE_WINDOW = 30;  // Frames to scan for exact boundaries (wider to catch earlier logo appearance)
   const double REFINE_THRESHOLD_OFFSET = 0.05;  // Use threshold - this during refinement to catch boundary frames
   const double REFINE_THRESHOLD_OFFSET_START = 0.08;  // More aggressive for start refinement (fade-in frames)
  const int CONFIRM_MISSING_FRAMES = 6;   // On a miss, scan a few real frames to confirm logo is truly gone (helps with layout switches)
   
   // Single pass: each frame read once, run all detections on it
   for (int frame_num = 0; frame_num < frame_count; frame_num += sample_interval) {
     cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);
     cv::Mat frame;
     if (!cap.read(frame)) break;
     
     for (size_t idx = 0; idx < states.size(); ++idx) {
       DetectionState& ds = states[idx];
       int found_x, found_y;
       double confidence;
       bool found = detect_logo_in_frame(frame, ds.template_img, ds.effective_region,
                                         ds.match_threshold, found_x, found_y, confidence, use_multi_scale);
       
       if (found) {
         if (!ds.currently_detecting) {
           ds.currently_detecting = true;
           ds.detection_start_frame = frame_num;
          ds.last_found_frame = frame_num;
           // Refine start: scan backward with slightly lower threshold to find first frame where logo appears
           double refine_threshold = std::max(0.35, ds.match_threshold - REFINE_THRESHOLD_OFFSET_START);
           int refine_start = std::max(0, frame_num - BOUNDARY_REFINE_WINDOW);
           for (int b = frame_num - 1; b >= refine_start; --b) {
             cap.set(cv::CAP_PROP_POS_FRAMES, b);
             cv::Mat bframe;
             if (!cap.read(bframe)) break;
             int bx, by;
             double bconf;
             if (detect_logo_in_frame(bframe, ds.template_img, ds.effective_region,
                                      refine_threshold, bx, by, bconf, use_multi_scale))
               ds.detection_start_frame = b;
             else
               break;
           }
           cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);
           std::cout << "  [" << ds.name << "] Logo FOUND at frame " << ds.detection_start_frame
                     << " pos(" << found_x << "," << found_y << ") " << (confidence * 100) << "%" << std::endl;
         }
         ds.last_detected_x = found_x;
         ds.last_detected_y = found_y;
         ds.last_confidence = confidence;
        ds.last_found_frame = frame_num;
         ds.consecutive_misses = 0;
       } else {
         if (ds.currently_detecting) {
           ds.consecutive_misses++;

          // Confirm the logo is truly gone (avoid long overlay persistence when layout switches and sample_interval is large).
          // Important: when sample_interval is large, the layout can change *within* the gap. So we check the last few
          // real frames right before the current miss frame, not just immediately after the last found frame.
          bool confirmed_missing = false;
          if (ds.last_found_frame >= 0 && ds.last_found_frame < frame_num) {
            confirmed_missing = true;
            const double confirm_threshold = std::max(0.35, ds.match_threshold - REFINE_THRESHOLD_OFFSET);
            const int confirm_start = std::max(ds.last_found_frame + 1, std::max(0, frame_num - CONFIRM_MISSING_FRAMES));
            const int confirm_end = frame_num - 1;
            for (int f = confirm_start; f <= confirm_end; ++f) {
              cap.set(cv::CAP_PROP_POS_FRAMES, f);
              cv::Mat cframe;
              if (!cap.read(cframe)) break;
              int cx, cy;
              double cconf;
              if (detect_logo_in_frame(cframe, ds.template_img, ds.effective_region,
                                       confirm_threshold, cx, cy, cconf, use_multi_scale)) {
                confirmed_missing = false;
                break;
              }
            }
            cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);
          }

          if (confirmed_missing) {
            int end_frame = ds.last_found_frame;
            // Refine end: scan forward with slightly lower threshold to find last frame where logo still appears
            double refine_threshold_end = std::max(0.35, ds.match_threshold - REFINE_THRESHOLD_OFFSET);
            int refine_end = std::min(frame_count - 1, end_frame + BOUNDARY_REFINE_WINDOW);
            for (int f = end_frame + 1; f <= refine_end; ++f) {
              cap.set(cv::CAP_PROP_POS_FRAMES, f);
              cv::Mat fframe;
              if (!cap.read(fframe)) break;
              int fx, fy;
              double fconf;
              if (detect_logo_in_frame(fframe, ds.template_img, ds.effective_region,
                                       refine_threshold_end, fx, fy, fconf, use_multi_scale)) {
                end_frame = f;
                ds.last_detected_x = fx;
                ds.last_detected_y = fy;
              } else {
                break;
              }
            }
            cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);

            int start_frame = std::max(0, ds.detection_start_frame - START_PADDING_FRAMES);
            end_frame = std::min(frame_count - 1, end_frame + END_PADDING_FRAMES);  // Small end padding only

            DetectionResult result;
            result.found = true;
            result.is_moving = false;  // Static logo
            result.x = ds.last_detected_x;
            result.y = ds.last_detected_y;
            result.width = ds.template_img.cols;
            result.height = ds.template_img.rows;
            result.confidence = ds.last_confidence;
            result.start_frame = start_frame;
            result.end_frame = end_frame;
            result.replacement_image = ds.replacement_image;
            result.scale = ds.scale;
            result.name = ds.name;
            result.full_screen = ds.full_screen;
            result.suppress_during_full_screen = ds.suppress_during_full_screen;
            results.push_back(result);
            std::cout << "  [" << ds.name << "] Segment: frames " << start_frame << "-" << end_frame << std::endl;

            ds.currently_detecting = false;
            ds.consecutive_misses = 0;
            continue;
          }

           if (ds.consecutive_misses >= MAX_CONSECUTIVE_MISSES) {
             int end_frame = frame_num - (ds.consecutive_misses * sample_interval);
             // Refine end: scan forward with slightly lower threshold to find last frame where logo still appears
             double refine_threshold_end = std::max(0.35, ds.match_threshold - REFINE_THRESHOLD_OFFSET);
             int refine_end = std::min(frame_count - 1, end_frame + BOUNDARY_REFINE_WINDOW);
             for (int f = end_frame + 1; f <= refine_end; ++f) {
               cap.set(cv::CAP_PROP_POS_FRAMES, f);
               cv::Mat fframe;
               if (!cap.read(fframe)) break;
               int fx, fy;
               double fconf;
               if (detect_logo_in_frame(fframe, ds.template_img, ds.effective_region,
                                       refine_threshold_end, fx, fy, fconf, use_multi_scale)) {
                 end_frame = f;
                 ds.last_detected_x = fx;
                 ds.last_detected_y = fy;
               } else
                 break;
             }
             cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);
             int start_frame = std::max(0, ds.detection_start_frame - START_PADDING_FRAMES);
             end_frame = std::min(frame_count - 1, end_frame + END_PADDING_FRAMES);  // Small end padding only
             DetectionResult result;
             result.found = true;
             result.is_moving = false;  // Static logo
             result.x = ds.last_detected_x;
             result.y = ds.last_detected_y;
             result.width = ds.template_img.cols;
             result.height = ds.template_img.rows;
             result.confidence = ds.last_confidence;
             result.start_frame = start_frame;
             result.end_frame = end_frame;
             result.replacement_image = ds.replacement_image;
             result.scale = ds.scale;
             result.name = ds.name;
             result.full_screen = ds.full_screen;
             result.suppress_during_full_screen = ds.suppress_during_full_screen;
             results.push_back(result);
             std::cout << "  [" << ds.name << "] Segment: frames " << start_frame << "-" << end_frame << std::endl;
             ds.currently_detecting = false;
             ds.consecutive_misses = 0;
           }
         }
       }
     }
   }
   
   // Close segments that run to end of video
   for (DetectionState& ds : states) {
     if (ds.currently_detecting) {
      int start_frame = std::max(0, ds.detection_start_frame - START_PADDING_FRAMES);
      DetectionResult result;
      result.found = true;
      result.is_moving = false;  // Static logo
      result.x = ds.last_detected_x;
      result.y = ds.last_detected_y;
      result.width = ds.template_img.cols;
      result.height = ds.template_img.rows;
      result.confidence = 0.0;
      result.start_frame = start_frame;
      result.end_frame = frame_count - 1;
      result.replacement_image = ds.replacement_image;
      result.scale = ds.scale;
      result.name = ds.name;
      result.full_screen = ds.full_screen;
      result.suppress_during_full_screen = ds.suppress_during_full_screen;
       results.push_back(result);
       std::cout << "  [" << ds.name << "] Segment: frames " << start_frame << "-" << (frame_count - 1) << " (end of video)" << std::endl;
     }
   }
   
   // Merge only very short gaps (same replacement) to avoid overlay dropouts; do not bridge layout changes (e.g. author full-screen)
   const int MAX_SEGMENT_GAP_FRAMES = 20;
   for (size_t i = 0; i < results.size(); ) {
     if (i + 1 >= results.size()) break;
     const auto& a = results[i];
     const auto& b = results[i + 1];
     int gap = b.start_frame - a.end_frame;
     if (a.replacement_image == b.replacement_image && a.x == b.x && a.y == b.y
         && gap <= MAX_SEGMENT_GAP_FRAMES && gap >= 0) {
       results[i].end_frame = b.end_frame;
       results.erase(results.begin() + static_cast<std::ptrdiff_t>(i + 1));
       continue;
     }
     ++i;
   }
   
   cap.release();
   return results;
 }
 
 
bool process_video(const std::string& input_path,
                   const std::string& output_path,
                   fg::VideoLayoutManager& layout_manager,
                   const std::string& layout_id,
                   bool auto_detect,
                   const std::string& ffmpeg_path,
                   int sample_interval,
                   bool dry_run,
                   bool use_multi_scale,
                   bool track_moving)
{
  auto time_start = std::chrono::steady_clock::now();
  auto stage_cursor = time_start;
  double stage_video_info_sec = 0.0;
  double stage_layout_select_sec = 0.0;
  double stage_detection_sec = 0.0;
  double stage_hole_punch_sec = 0.0;
  double stage_strategy_prep_sec = 0.0;
  double stage_pass1_sec = 0.0;
  double stage_pass2_sec = 0.0;
  std::cout << "\nProcessing: " << input_path << std::endl;
  
  // Get video info
  int width, height, frame_count;
  double fps;
  if (!get_video_info(input_path, width, height, frame_count, fps)) {
    std::cerr << "  Error: Cannot open video file" << std::endl;
    std::cout << "  Total time: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - time_start).count() << "s" << std::endl;
    return false;
  }
  stage_video_info_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_cursor).count();
  stage_cursor = std::chrono::steady_clock::now();
  
  std::cout << "  Resolution: " << width << "x" << height << std::endl;
  std::cout << "  Frames: " << frame_count << ", FPS: " << fps << std::endl;
  
  // Find matching layout
  fg::layout_ptr layout;
  if (auto_detect) {
    layout = layout_manager.get_layout_for_resolution(width, height);
    if (!layout) {
      std::cerr << "  Error: No layout found for resolution " << width << "x" << height << std::endl;
      std::cout << "  Total time: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - time_start).count() << "s" << std::endl;
      return false;
    }
    std::cout << "  Auto-detected layout: " << layout->name << std::endl;
  } else {
    layout = layout_manager.get_layout(layout_id);
    if (!layout) {
      std::cerr << "  Error: Layout '" << layout_id << "' not found" << std::endl;
      std::cout << "  Total time: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - time_start).count() << "s" << std::endl;
      return false;
    }
    std::cout << "  Using layout: " << layout->name << std::endl;
  }
  stage_layout_select_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_cursor).count();
  stage_cursor = std::chrono::steady_clock::now();
  
  // Check if detection is enabled and layout has detections
  std::vector<DetectionResult> detections;
  
  if (layout_manager.is_detection_enabled() && layout->uses_detection()) {
    std::cout << "  Running template matching detection..." << std::endl;
    
    // NEW: Use hybrid detection if track_moving enabled
    if (track_moving) {
      detections = detect_logos_hybrid(input_path, layout_manager, *layout, sample_interval, use_multi_scale);
      bool has_moving = false;
      for (const auto& d : detections) {
        if (d.is_moving) {
          has_moving = true;
          break;
        }
      }
      if (!has_moving) {
        std::cout << "  [TRACK-MOVING] No moving segments detected; using legacy static detector for stable static behavior" << std::endl;
        auto static_only = detect_logos_in_video(input_path, layout_manager, *layout, sample_interval, use_multi_scale);
        if (!static_only.empty()) {
          detections = std::move(static_only);
        } else {
          std::cout << "  [TRACK-MOVING] Static fallback returned no segments; keeping hybrid static segments" << std::endl;
        }
      }
    } else {
      detections = detect_logos_in_video(input_path, layout_manager, *layout, sample_interval, use_multi_scale);
    }
    stage_detection_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_cursor).count();
    stage_cursor = std::chrono::steady_clock::now();
     
     if (detections.empty()) {
       std::cout << "  No logos detected in video, skipping" << std::endl;
       std::cout << "  Total time: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - time_start).count() << "s" << std::endl;
       return true;
     }
     
     std::cout << "  Detected " << detections.size() << " logo segment(s)" << std::endl;
   } else if (!layout->replacements.empty()) {
     // Fall back to position-based replacement
     std::cout << "  Using position-based replacement (no detection)" << std::endl;
     for (const auto& repl : layout->replacements) {
      DetectionResult result;
      result.found = true;
      result.is_moving = false;  // Static position-based
      result.x = repl.x;
      result.y = repl.y;
      result.width = repl.width;
      result.height = repl.height;
      result.start_frame = 0;
      result.end_frame = frame_count - 1;
      result.replacement_image = repl.replacement_image;
      result.scale = repl.scale;
      result.name = "";
      result.full_screen = false;
      result.suppress_during_full_screen = false;
       detections.push_back(result);
     }
   } else {
     std::cout << "  No detections or replacements configured" << std::endl;
     auto elapsed = std::chrono::steady_clock::now() - time_start;
     double sec = std::chrono::duration<double>(elapsed).count();
     std::cout << "  Total time: " << sec << "s" << std::endl;
     return true;
   }
   
   // Punch holes: do not draw overlays that have suppress_during_full_screen when a full_screen segment is active (config-driven)
   {
    auto hole_start = std::chrono::steady_clock::now();
     std::vector<std::pair<int, int>> full_screen_ranges;
     for (const auto& d : detections) {
       if (d.full_screen)
         full_screen_ranges.push_back({ d.start_frame, d.end_frame });
     }
     if (!full_screen_ranges.empty()) {
       std::vector<DetectionResult> out;
       for (const auto& d : detections) {
         if (!d.suppress_during_full_screen) {
           out.push_back(d);
           continue;
         }
         // Subtract full-screen ranges from [d.start_frame, d.end_frame]
         std::vector<std::pair<int, int>> ranges = { { d.start_frame, d.end_frame } };
         for (const auto& fs : full_screen_ranges) {
           std::vector<std::pair<int, int>> next;
           for (const auto& r : ranges) {
             int s = r.first, e = r.second;
             int o_start = std::max(s, fs.first), o_end = std::min(e, fs.second);
             if (o_start > o_end) {
               next.push_back(r);
               continue;
             }
             if (s <= o_start - 1) next.push_back({ s, o_start - 1 });
             if (o_end + 1 <= e) next.push_back({ o_end + 1, e });
           }
           ranges = std::move(next);
         }
         for (const auto& r : ranges) {
           DetectionResult copy = d;
           copy.start_frame = r.first;
           copy.end_frame = r.second;
           out.push_back(copy);
         }
       }
       detections = std::move(out);
     }
    stage_hole_punch_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - hole_start).count();
   }

  // ================================================================
  // HYBRID PROCESSING: Separate STATIC and MOVING segments
  // If any MOVING → single-pass OpenCV for ALL overlays (avoids frame mismatch)
  // If all STATIC → fast FFmpeg overlay
  // ================================================================
  std::vector<DetectionResult> static_dets, moving_dets;
  for (const auto& det : detections) {
    if (det.is_moving) {
      moving_dets.push_back(det);
    } else if (!det.full_screen) {  // Skip full_screen markers for processing
      static_dets.push_back(det);
    }
  }
  
  std::cout << "\n  --- Processing Strategy ---" << std::endl;
  std::cout << "  Static segments: " << static_dets.size() << std::endl;
  std::cout << "  Moving segments: " << moving_dets.size() << std::endl;
  stage_strategy_prep_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_cursor).count();
  
  if (!moving_dets.empty()) {
    // Mixed case:
    //   1) apply MOVING overlays exactly as before (OpenCV on original input)
    //   2) apply STATIC overlays with legacy FFmpeg logic
    //   3) ensure original audio is preserved
    std::cout << "  Mode: Legacy STATIC (FFmpeg) + MOVING (OpenCV)" << std::endl;

    for (const auto& sd : static_dets) {
      std::cout << "    STATIC [" << sd.name << "] frames " << sd.start_frame << "-" << sd.end_frame
                << " pos=(" << sd.x << "," << sd.y << ")" << std::endl;
    }
    for (const auto& md : moving_dets) {
      std::cout << "    MOVING [" << md.name << "] frames " << md.start_frame << "-" << md.end_frame
                << " tracked=" << md.tracked_positions.size() << " positions" << std::endl;
    }

    if (dry_run) {
      std::cout << "  [DRY RUN] Would run OpenCV MOVING pass first, then legacy STATIC FFmpeg pass" << std::endl;
      auto elapsed = std::chrono::steady_clock::now() - time_start;
      std::cout << "  Total time: " << std::chrono::duration<double>(elapsed).count() << "s" << std::endl;
      return true;
    }

    // PASS 1/3: Apply only MOVING overlays via OpenCV frame tracking on original input
    std::cout << "\n  --- PASS 1/3: Frame-by-frame for MOVING overlays (" << moving_dets.size() << ") ---" << std::endl;
    std::string temp_video_path = output_path + ".video_only.mp4";
    auto moving_pass_start = std::chrono::steady_clock::now();
    bool moving_ok = process_video_with_moving_logos(input_path, temp_video_path, moving_dets, layout_manager, fps);
    double moving_pass_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - moving_pass_start).count();
    stage_pass1_sec = moving_pass_sec;

    if (!moving_ok) {
      std::cerr << "  Error: MOVING OpenCV pass failed" << std::endl;
      std::remove(temp_video_path.c_str());
      return false;
    }
    std::cout << "  PASS 1 complete in " << std::fixed << std::setprecision(1) << moving_pass_sec << "s" << std::endl;

    // PASS 2/3 + 3/3:
    // Apply STATIC overlays via legacy FFmpeg command on top of moving-rendered video and keep original audio.
    auto pass23_start = std::chrono::steady_clock::now();
    if (!static_dets.empty()) {
      std::cout << "\n  --- PASS 2/3: Legacy FFmpeg for STATIC overlays (on moving-rendered video) ---" << std::endl;
      std::stringstream static_cmd;
      static_cmd << ffmpeg_path << " -y";
      static_cmd << " -i \"" << temp_video_path << "\""; // video from moving pass
      static_cmd << " -i \"" << input_path << "\"";      // original audio source

      std::vector<std::string> replacement_images;
      for (const auto& det : static_dets) {
        std::string img_path = layout_manager.get_image_path(det.replacement_image);
        bool already_added = false;
        for (const auto& existing : replacement_images) {
          if (existing == img_path) {
            already_added = true;
            break;
          }
        }
        if (!already_added) {
          replacement_images.push_back(img_path);
          static_cmd << " -i \"" << img_path << "\"";
        }
      }

      static_cmd << " -filter_complex \"";
      std::string prev_output = "[0:v]";
      for (size_t i = 0; i < static_dets.size(); ++i) {
        const auto& det = static_dets[i];
        std::string img_path = layout_manager.get_image_path(det.replacement_image);
        int img_index = 2; // inputs: 0=moving video, 1=original, images start at 2
        for (size_t j = 0; j < replacement_images.size(); ++j) {
          if (replacement_images[j] == img_path) {
            img_index = static_cast<int>(j + 2);
            break;
          }
        }

        int scaled_w = static_cast<int>(det.width * det.scale);
        int scaled_h = static_cast<int>(det.height * det.scale);
        std::string scaled_label = "[scaled_static_" + std::to_string(i) + "]";
        std::string next_output = (i == static_dets.size() - 1)
          ? "[out_v]"
          : "[tmp_static_" + std::to_string(i) + "]";

        static_cmd << "[" << img_index << ":v]scale=" << scaled_w << ":" << scaled_h << scaled_label << ";";
        static_cmd << prev_output << scaled_label << "overlay=" << det.x << ":" << det.y;
        static_cmd << ":enable='between(n," << det.start_frame << "," << det.end_frame << "')"
                   << next_output;
        if (i < static_dets.size() - 1) {
          static_cmd << ";";
        }
        prev_output = next_output;
      }
      static_cmd << ";[1:a]anull[out_a]\"";
      static_cmd << " -map \"[out_v]\" -map \"[out_a]\"";
      static_cmd << " -c:v libx264 -preset medium -crf 18";
      static_cmd << " -c:a aac -b:a 192k";
      static_cmd << " \"" << output_path << "\"";

      std::cout << "  CMD: " << static_cmd.str() << std::endl;
      int ret = system(static_cmd.str().c_str());
      if (ret != 0) {
        std::cerr << "  Warning: STATIC pass failed (code " << ret << "), falling back to audio merge only" << std::endl;
        std::stringstream audio_cmd;
        audio_cmd << ffmpeg_path << " -y -i \"" << temp_video_path << "\" -i \"" << input_path << "\"";
        audio_cmd << " -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k";
        audio_cmd << " \"" << output_path << "\"";
        int audio_ret = system(audio_cmd.str().c_str());
        if (audio_ret != 0) {
          std::cerr << "  Error: Audio merge fallback failed (code " << audio_ret << ")" << std::endl;
          std::remove(temp_video_path.c_str());
          return false;
        }
      }
    } else {
      // No static overlays; only merge original audio.
      std::cout << "\n  --- PASS 2/3: No STATIC overlays ---" << std::endl;
      std::cout << "  --- PASS 3/3: Merging audio ---" << std::endl;
      std::stringstream audio_cmd;
      audio_cmd << ffmpeg_path << " -y -i \"" << temp_video_path << "\" -i \"" << input_path << "\"";
      audio_cmd << " -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k";
      audio_cmd << " \"" << output_path << "\"";
      std::cout << "  CMD: " << audio_cmd.str() << std::endl;
      int ret = system(audio_cmd.str().c_str());
      if (ret != 0) {
        std::cerr << "  Error: Audio merge failed (code " << ret << ")" << std::endl;
        std::remove(temp_video_path.c_str());
        return false;
      }
    }
    stage_pass2_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - pass23_start).count();
    std::cout << "  PASS 2+3 complete in " << std::fixed << std::setprecision(1) << stage_pass2_sec << "s" << std::endl;
    
    // Cleanup temp files
    std::remove(temp_video_path.c_str());
    
    auto elapsed = std::chrono::steady_clock::now() - time_start;
    double sec = std::chrono::duration<double>(elapsed).count();
    double known = stage_video_info_sec + stage_layout_select_sec + stage_detection_sec +
                   stage_hole_punch_sec + stage_strategy_prep_sec + stage_pass1_sec + stage_pass2_sec;
    double unaccounted = std::max(0.0, sec - known);
    std::cout << "\n  --- Latency Breakdown (video-level) ---" << std::endl;
    std::cout << "    Video info:         " << std::fixed << std::setprecision(3) << stage_video_info_sec << "s" << std::endl;
    std::cout << "    Layout select:      " << stage_layout_select_sec << "s" << std::endl;
    std::cout << "    Detection total:    " << stage_detection_sec << "s" << std::endl;
    std::cout << "    Hole-punch filters: " << stage_hole_punch_sec << "s" << std::endl;
    std::cout << "    Strategy prep:      " << stage_strategy_prep_sec << "s" << std::endl;
    std::cout << "    PASS 1 (moving OpenCV): " << stage_pass1_sec << "s" << std::endl;
    std::cout << "    PASS 2+3 (static+audio): " << stage_pass2_sec << "s" << std::endl;
    std::cout << "    ----------------------------------" << std::endl;
    std::cout << "    Accounted subtotal: " << known << "s" << std::endl;
    std::cout << "    Unaccounted:        " << unaccounted << "s" << std::endl;
    std::cout << "\n  --- PROCESSING COMPLETE ---" << std::endl;
    std::cout << "  Total time: " << std::fixed << std::setprecision(1) << sec << "s" << std::endl;
    std::cout << "  Output: " << output_path << std::endl;
    return true;
  }
  
  // If we reach here, all detections are STATIC — use fast FFmpeg path
  std::cout << "  Mode: FFmpeg only (all segments are static)" << std::endl;
  
  // Put only the non-full-screen static detections back for FFmpeg processing
  detections = static_dets;
  
  // (Prevents generating an empty filtergraph that starts with ';' and missing [out_v].)
  if (detections.empty()) {
    std::stringstream cmd;
    cmd << ffmpeg_path << " -y";
    cmd << " -i \"" << input_path << "\"";
    cmd << " -filter_complex \"[0:v]null[out_v];[0:a]anull[out_a]\"";
    cmd << " -map \"[out_v]\" -map \"[out_a]\"";
    cmd << " -c:v libx264 -preset medium -crf 18";
    cmd << " -c:a aac -b:a 192k";
    cmd << " \"" << output_path << "\"";
    std::cout << "\n  FFmpeg command:\n  " << cmd.str() << std::endl;
    if (dry_run) {
      std::cout << "  [DRY RUN] Would execute above command" << std::endl;
      auto elapsed = std::chrono::steady_clock::now() - time_start;
      std::cout << "  Total time: " << std::chrono::duration<double>(elapsed).count() << "s" << std::endl;
      return true;
    }
    std::cout << "\n  Running FFmpeg..." << std::endl;
    int result = system(cmd.str().c_str());
    auto elapsed = std::chrono::steady_clock::now() - time_start;
    double sec = std::chrono::duration<double>(elapsed).count();
    std::cout << "  Total time: " << sec << "s" << std::endl;
    if (result == 0) {
      std::cout << "  Output: " << output_path << std::endl;
      return true;
    }
    std::cerr << "  Error: FFmpeg failed with code " << result << std::endl;
    return false;
  }
   
  // Build FFmpeg command with detected segments
   std::stringstream cmd;
   cmd << ffmpeg_path << " -y";
   
   // Add video input FIRST
   cmd << " -i \"" << input_path << "\"";
   
   // Add replacement image inputs
   std::vector<std::string> replacement_images;
   for (const auto& det : detections) {
     std::string img_path = layout_manager.get_image_path(det.replacement_image);
     // Check if we already added this image
     bool already_added = false;
     for (const auto& existing : replacement_images) {
       if (existing == img_path) {
         already_added = true;
         break;
       }
     }
     if (!already_added) {
       replacement_images.push_back(img_path);
       cmd << " -i \"" << img_path << "\"";
     }
   }
   
   // Build filter complex
   cmd << " -filter_complex \"";
   
   std::string prev_output = "[0:v]";
   for (size_t i = 0; i < detections.size(); ++i) {
     const auto& det = detections[i];
     
     // Find the input index for this replacement image
     std::string img_path = layout_manager.get_image_path(det.replacement_image);
     int img_index = 1;  // Start at 1 since video is 0
     for (size_t j = 0; j < replacement_images.size(); ++j) {
       if (replacement_images[j] == img_path) {
         img_index = j + 1;
         break;
       }
     }
     
     // Scale the replacement image
     int scaled_w = static_cast<int>(det.width * det.scale);
     int scaled_h = static_cast<int>(det.height * det.scale);
     
     std::string scaled_label = "[scaled" + std::to_string(i) + "]";
     cmd << "[" << img_index << ":v]scale=" << scaled_w << ":" << scaled_h << scaled_label << ";";
     
     // Apply overlay with enable expression
     std::string next_output = (i == detections.size() - 1) ? "[out_v]" : "[tmp" + std::to_string(i) + "]";
     
     cmd << prev_output << scaled_label << "overlay=" << det.x << ":" << det.y;
     cmd << ":enable='between(n," << det.start_frame << "," << det.end_frame << ")'";
     cmd << next_output;
     
     if (i < detections.size() - 1) {
       cmd << ";";
     }
     
     prev_output = next_output;
   }
   
   // Audio passthrough
   cmd << ";[0:a]anull[out_a]\"";
   
   // Map outputs
   cmd << " -map \"[out_v]\" -map \"[out_a]\"";
   
   // Output encoding settings
   cmd << " -c:v libx264 -preset medium -crf 18";
   cmd << " -c:a aac -b:a 192k";
   cmd << " \"" << output_path << "\"";
   
   std::cout << "\n  FFmpeg command:\n  " << cmd.str() << std::endl;
   
   if (dry_run) {
     std::cout << "  [DRY RUN] Would execute above command" << std::endl;
     auto elapsed = std::chrono::steady_clock::now() - time_start;
     std::cout << "  Total time: " << std::chrono::duration<double>(elapsed).count() << "s" << std::endl;
     return true;
   }
   
   // Execute FFmpeg
   std::cout << "\n  Running FFmpeg..." << std::endl;
   int result = system(cmd.str().c_str());
   
   auto elapsed = std::chrono::steady_clock::now() - time_start;
   double sec = std::chrono::duration<double>(elapsed).count();
   std::cout << "  Total time: " << sec << "s" << std::endl;
   if (result == 0) {
     std::cout << "  Output: " << output_path << std::endl;
     return true;
   } else {
     std::cerr << "  Error: FFmpeg failed with code " << result << std::endl;
     return false;
   }
 }
 
 
 int main(int argc, char* argv[])
 {
  std::string input_folder;
  std::string output_folder;
  std::string config_path;
  std::string layout_id = "auto";
  std::string ffmpeg_path = "ffmpeg";
  bool auto_detect = false;
  bool dry_run = false;
  bool use_multi_scale = true;
  bool track_moving = false;  // NEW: Enable moving logo tracking
  int sample_interval = 30;  // Check every 30 frames by default
  
  // Parse arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--input-folder" && i + 1 < argc) {
      input_folder = argv[++i];
    } else if (arg == "--output-folder" && i + 1 < argc) {
      output_folder = argv[++i];
    } else if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--layout" && i + 1 < argc) {
      layout_id = argv[++i];
    } else if (arg == "--ffmpeg" && i + 1 < argc) {
      ffmpeg_path = argv[++i];
    } else if (arg == "--sample-interval" && i + 1 < argc) {
      sample_interval = atoi(argv[++i]);
    } else if (arg == "--auto-detect") {
      auto_detect = true;
    } else if (arg == "--no-multi-scale") {
      use_multi_scale = false;
    } else if (arg == "--track-moving") {
      track_moving = true;
    } else if (arg == "--dry-run") {
      dry_run = true;
    } else {
      std::cerr << "Unknown option: " << arg << std::endl;
      print_usage(argv[0]);
      return 1;
    }
  }
   
   // Validate arguments
   if (input_folder.empty()) {
     std::cerr << "Error: --input-folder is required" << std::endl;
     print_usage(argv[0]);
     return 1;
   }
   
   if (output_folder.empty()) {
     std::cerr << "Error: --output-folder is required" << std::endl;
     print_usage(argv[0]);
     return 1;
   }
   
   if (config_path.empty()) {
     std::cerr << "Error: --config is required" << std::endl;
     print_usage(argv[0]);
     return 1;
   }
   
   // Create output folder if it doesn't exist
   mkdir(output_folder.c_str(), 0755);
   
   // Load config
   fg::VideoLayoutManager layout_manager;
   if (!layout_manager.load_from_file(config_path)) {
     std::cerr << "Error: Cannot load config file " << config_path << std::endl;
     return 1;
   }
   
   // Update directories relative to config file
   size_t pos = config_path.rfind('/');
   if (pos != std::string::npos) {
     std::string config_dir = config_path.substr(0, pos);
     std::string images_dir = config_dir + "/" + layout_manager.get_images_directory();
     std::string ref_dir = config_dir + "/" + layout_manager.get_reference_directory();
     layout_manager.set_images_directory(images_dir);
     layout_manager.set_reference_directory(ref_dir);
   }
   
  std::cout << "Loaded config: " << config_path << std::endl;
  std::cout << "Detection enabled: " << (layout_manager.is_detection_enabled() ? "YES" : "NO") << std::endl;
  std::cout << "Moving logo tracking: " << (track_moving ? "YES" : "NO") << std::endl;
  std::cout << "Images directory: " << layout_manager.get_images_directory() << std::endl;
  std::cout << "Reference directory: " << layout_manager.get_reference_directory() << std::endl;
  std::cout << "Sample interval: " << sample_interval << " frames" << std::endl;
  std::cout << "Layouts available: " << layout_manager.get_all_layouts().size() << std::endl;
   
   for (const auto& layout : layout_manager.get_all_layouts()) {
     std::cout << "  - " << layout.id << ": " << layout.name 
               << " (" << layout.resolution.width << "x" << layout.resolution.height << ")";
     if (layout.uses_detection()) {
       std::cout << " [" << layout.detections.size() << " detection(s)]";
     }
     std::cout << std::endl;
   }
   
   // List videos
   auto videos = list_video_files(input_folder);
   if (videos.empty()) {
     std::cerr << "No video files found in " << input_folder << std::endl;
     return 1;
   }
   
   std::cout << "\nFound " << videos.size() << " video(s) to process" << std::endl;
   
   // Process each video
   int success_count = 0;
   int error_count = 0;
   
   for (const auto& video_path : videos) {
    std::string output_path = get_output_filename(video_path, output_folder);
    
    if (process_video(video_path, output_path, layout_manager,
                      layout_id, auto_detect, ffmpeg_path, sample_interval, dry_run, use_multi_scale, track_moving)) {
      success_count++;
    } else {
      error_count++;
    }
   }
   
   std::cout << "\n========================================" << std::endl;
   std::cout << "Batch processing complete!" << std::endl;
   std::cout << "  Success: " << success_count << std::endl;
   std::cout << "  Errors: " << error_count << std::endl;
   std::cout << "========================================" << std::endl;
   
   return (error_count > 0) ? 1 : 0;
 }
 
 