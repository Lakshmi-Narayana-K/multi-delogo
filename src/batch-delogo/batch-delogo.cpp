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
 #include <chrono>
 #include <cmath>
 #include <cstdlib>
 #include <deque>
 #include <iomanip>
 #include <iostream>
 #include <fstream>
 #include <mutex>
 #include <sstream>
 #include <string>
 #include <thread>
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
             << "  --dry-run               Show what would be done without executing\n"
             << "  --help                  Show this help message\n"
             << "\n"
             << "Example:\n"
             << "  " << program_name << " --input-folder ./videos --output-folder ./output \\\n"
             << "                     --config video_layouts.json --auto-detect\n"
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
 
// ── Moving-logo structs + classifier ─────────────────────────────────────────
// Declared here (before detect_logos_in_video) so they are visible inside it.

struct MovPosSample { int frame; int x; int y; };

struct MovTrackState {
  std::string       name;
  cv::Mat           template_img;
  fg::SearchRegion  region;
  double            threshold;
  std::vector<MovPosSample> history;
  MovPosSample      pre_loss_pos{-1, 0, 0};
};

struct MovingEvent {
  std::string  video_path;
  std::string  logo_name;
  int          frame_num  = 0;
  double       fps        = 30.0;
  std::string  direction;
  double       net_dist   = 0.0;
};

// Returns true when 2+ consecutive coarse steps exceed threshold (Phase-2 rule)
static bool mov_classify(const std::vector<MovPosSample>& pts,
                         double threshold,
                         MovingEvent& out,
                         const std::string& logo_name,
                         const std::string& video_path,
                         double fps)
{
  if (pts.size() < 3) return false;

  std::vector<bool> moving(pts.size(), false);
  for (size_t i = 1; i < pts.size(); ++i) {
    double dx = pts[i].x - pts[i-1].x;
    double dy = pts[i].y - pts[i-1].y;
    if (std::sqrt(dx*dx + dy*dy) >= threshold) {
      moving[i-1] = moving[i] = true;
    }
  }

  std::vector<bool> dilated = moving;
  for (size_t i = 0; i < moving.size(); ++i) {
    if (!moving[i]) continue;
    if (i > 0)                  dilated[i-1] = true;
    if (i+1 < moving.size())    dilated[i+1] = true;
  }

  int run = 0;
  for (size_t i = 0; i < dilated.size(); ++i) {
    if (dilated[i]) {
      if (++run >= 2) {
        int fi = -1, li = -1;
        for (size_t j = 0; j < moving.size(); ++j)
          if (moving[j]) { if (fi < 0) fi = static_cast<int>(j); li = static_cast<int>(j); }
        const auto& fp = pts[fi >= 0 ? fi : 0];
        const auto& lp = pts[li >= 0 ? li : static_cast<int>(pts.size())-1];
        double ndx = lp.x - fp.x;
        double ndy = lp.y - fp.y;
        out.logo_name  = logo_name;
        out.video_path = video_path;
        out.frame_num  = fp.frame;
        out.fps        = fps;
        out.net_dist   = std::sqrt(ndx*ndx + ndy*ndy);
        out.direction  = (std::abs(ndx) > std::abs(ndy))
                         ? (ndx > 0 ? "left-to-right" : "right-to-left")
                         : (ndy > 0 ? "top-to-bottom" : "bottom-to-top");
        return true;
      }
    } else {
      run = 0;
    }
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────

// Detect logo presence: one pass over video, all detections per frame.
// Pure static detection — moving-logo tracking runs in a parallel thread.
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

   auto t_scan_start = std::chrono::steady_clock::now();

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

   double scan_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_scan_start).count();
   std::cout << std::fixed << std::setprecision(1)
             << "  [DETECT] Static detection scan: " << scan_secs << "s\n";

   cap.release();
   return results;
}
 
 
// Scan a video for moving logos (runs in its own thread parallel to detect_logos_in_video).
// Uses its own VideoCapture so there is no contention with the detection thread.
static std::vector<MovingEvent> scan_moving_logos(
    const std::string& video_path,
    fg::VideoLayoutManager& layout_mgr,
    const fg::VideoLayout& layout,
    double fps,
    int sample_interval)
{
  std::vector<MovingEvent> events;

  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) return events;

  int frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  int fw          = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int fh          = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

  std::vector<MovTrackState> tracks;
  for (const auto& det : layout.detections) {
    cv::Mat tmpl = cv::imread(layout_mgr.get_reference_path(det.reference_image));
    if (tmpl.empty()) continue;
    MovTrackState ts;
    ts.name         = det.name;
    ts.template_img = tmpl;
    ts.threshold    = det.match_threshold;
    if (det.search_quadrant >= 1 && det.search_quadrant <= 4)
      ts.region = fg::search_region_from_quadrant(fw, fh, det.search_quadrant);
    else if (det.search_region.width > 0 && det.search_region.height > 0)
      ts.region = det.search_region;
    else
      ts.region = fg::SearchRegion(0, 0, fw, fh);
    tracks.push_back(std::move(ts));
  }
  if (tracks.empty()) { cap.release(); return events; }

  const int    HISTORY_KEEP     = 6;
  const double MOTION_THRESHOLD = 12.0;
  const int    FINE_STEP        = 5;
  const int    MAX_LOSS_GAP     = sample_interval * 10;

  auto report = [&](const MovingEvent& ev) {
    int sec = static_cast<int>(ev.frame_num / std::max(ev.fps, 0.01));
    std::cout << "  [MOVING] " << ev.logo_name
              << " at " << sec/60 << "m " << std::setw(2) << std::setfill('0') << sec%60 << "s"
              << std::setfill(' ')
              << " | direction: " << ev.direction
              << " | shift: " << std::fixed << std::setprecision(1) << ev.net_dist << " px\n";
    events.push_back(ev);
  };

  auto t_start = std::chrono::steady_clock::now();

  for (int fn = 0; fn < frame_count; fn += sample_interval) {
    cap.set(cv::CAP_PROP_POS_FRAMES, fn);
    cv::Mat frame;
    if (!cap.read(frame)) break;

    for (auto& ts : tracks) {
      int x = 0, y = 0; double conf = 0.0;
      bool found = detect_logo_in_frame(frame, ts.template_img, ts.region,
                                        ts.threshold, x, y, conf, false);
      if (found) {
        // Position-jump check: logo reappears at very different position after a gap
        if (ts.history.empty() && ts.pre_loss_pos.frame >= 0) {
          double dx   = static_cast<double>(x - ts.pre_loss_pos.x);
          double dy   = static_cast<double>(y - ts.pre_loss_pos.y);
          double dist = std::sqrt(dx*dx + dy*dy);
          int    gap  = fn - ts.pre_loss_pos.frame;
          if (dist >= MOTION_THRESHOLD && gap <= MAX_LOSS_GAP) {
            MovingEvent ev;
            ev.video_path = video_path;
            ev.logo_name  = ts.name;
            ev.frame_num  = ts.pre_loss_pos.frame;
            ev.fps        = fps;
            ev.net_dist   = dist;
            ev.direction  = (std::abs(dx) > std::abs(dy))
                            ? (dx > 0 ? "left-to-right" : "right-to-left")
                            : (dy > 0 ? "top-to-bottom" : "bottom-to-top");
            report(ev);
          }
          ts.pre_loss_pos = {-1, 0, 0};
        }
        ts.history.push_back({fn, x, y});
        if (static_cast<int>(ts.history.size()) > HISTORY_KEEP)
          ts.history.erase(ts.history.begin());
        MovingEvent ev;
        if (mov_classify(ts.history, MOTION_THRESHOLD, ev, ts.name, video_path, fps)) {
          report(ev);
          ts.history.clear();
        }
      } else {
        // Logo lost: fine-scan gap to catch fast transitions
        if (!ts.history.empty()) {
          ts.pre_loss_pos = ts.history.back();
          int fine_start = ts.history.back().frame;
          int fine_end   = std::min(frame_count - 1, fn + sample_interval);
          std::vector<MovPosSample> fine_pts;
          cap.set(cv::CAP_PROP_POS_FRAMES, fine_start);
          for (int ff = fine_start; ff <= fine_end; ff += FINE_STEP) {
            cap.set(cv::CAP_PROP_POS_FRAMES, ff);
            cv::Mat f;
            if (!cap.read(f)) break;
            int fx = 0, fy = 0; double fc = 0.0;
            if (detect_logo_in_frame(f, ts.template_img, ts.region,
                                     ts.threshold, fx, fy, fc, false)) {
              fine_pts.push_back({ff, fx, fy});
              if (static_cast<int>(fine_pts.size()) > HISTORY_KEEP)
                fine_pts.erase(fine_pts.begin());
              MovingEvent fev;
              if (mov_classify(fine_pts, MOTION_THRESHOLD, fev, ts.name, video_path, fps)) {
                report(fev);
                break;
              }
            }
          }
          cap.set(cv::CAP_PROP_POS_FRAMES, fn);
        }
        ts.history.clear();
      }
    }
  }

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
  std::cout << std::fixed << std::setprecision(1)
            << "  [MOVING] Moving-logo scan: " << secs << "s"
            << "  [" << events.size() << " event(s) found]\n";

  cap.release();
  return events;
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
                   std::vector<MovingEvent>& all_moving_events)
 {
   auto time_start = std::chrono::steady_clock::now();
   std::cout << "\nProcessing: " << input_path << std::endl;
   
   // Get video info
   int width, height, frame_count;
   double fps;
   if (!get_video_info(input_path, width, height, frame_count, fps)) {
     std::cerr << "  Error: Cannot open video file" << std::endl;
     std::cout << "  Total time: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - time_start).count() << "s" << std::endl;
     return false;
   }
   
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
   
   // Check if detection is enabled and layout has detections
   std::vector<DetectionResult> detections;
   
   if (layout_manager.is_detection_enabled() && layout->uses_detection()) {
     std::cout << "  Running detection + moving-logo scan in parallel..." << std::endl;
     auto t_phase1 = std::chrono::steady_clock::now();

     // Thread 1: static logo detection (finds segments, positions, boundaries)
     // Thread 2: moving logo scan (tracks per-frame movement)
     // Each thread opens its own VideoCapture — no shared state.
     std::vector<MovingEvent> video_moving_events;
     std::thread t_moving([&]() {
       video_moving_events = scan_moving_logos(input_path, layout_manager, *layout,
                                               fps, sample_interval);
     });
     // Run static detection on this thread while t_moving runs on the other.
     detections = detect_logos_in_video(input_path, layout_manager, *layout,
                                        sample_interval, use_multi_scale);
     t_moving.join();

     double phase1_secs = std::chrono::duration<double>(
       std::chrono::steady_clock::now() - t_phase1).count();
     std::cout << std::fixed << std::setprecision(1)
               << "  Phase 1 (parallel wall-clock): " << phase1_secs << "s\n";

     // Accumulate moving events into the shared summary list
     for (const auto& ev : video_moving_events)
       all_moving_events.push_back(ev);

     if (video_moving_events.empty())
       std::cout << "  No moving logos detected." << std::endl;
     else
       std::cout << "  Moving logos found: " << video_moving_events.size()
                 << " event(s) in this video." << std::endl;

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
   }

  // If suppression removed all overlays, just passthrough video+audio.
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
   auto t_ffmpeg = std::chrono::steady_clock::now();
   int result = system(cmd.str().c_str());
   double ffmpeg_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_ffmpeg).count();

   auto elapsed = std::chrono::steady_clock::now() - time_start;
   double total_sec = std::chrono::duration<double>(elapsed).count();
   std::cout << std::fixed << std::setprecision(1)
             << "  Phase 2 (FFmpeg encoding): " << ffmpeg_secs << "s\n"
             << "  Total time: " << total_sec << "s\n";
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
  std::vector<MovingEvent> all_moving_events;

  for (const auto& video_path : videos) {
    std::string output_path = get_output_filename(video_path, output_folder);
    
    if (process_video(video_path, output_path, layout_manager,
                      layout_id, auto_detect, ffmpeg_path, sample_interval, dry_run,
                      use_multi_scale, all_moving_events)) {
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

  // ── Moving-logo summary ───────────────────────────────────────────────────
  if (!all_moving_events.empty()) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "MOVING LOGO SUMMARY — " << all_moving_events.size()
              << " event(s) detected:" << std::endl;
    std::string cur_video;
    for (const auto& ev : all_moving_events) {
      if (ev.video_path != cur_video) {
        cur_video = ev.video_path;
        size_t sl = cur_video.rfind('/');
        std::string fname = (sl != std::string::npos) ? cur_video.substr(sl+1) : cur_video;
        std::cout << "\n  Video: " << fname << std::endl;
      }
      int sec = static_cast<int>(ev.frame_num / std::max(ev.fps, 0.01));
      std::cout << "    " << sec/60 << "m " << std::setw(2) << std::setfill('0') << sec%60 << "s"
                << std::setfill(' ')
                << "  " << ev.logo_name
                << "  [" << ev.direction << "]"
                << "  shift: " << std::fixed << std::setprecision(1) << ev.net_dist << " px\n";
    }
    std::cout << "========================================" << std::endl;
  } else {
    std::cout << "\nNo moving logos detected in any video." << std::endl;
  }

  return (error_count > 0) ? 1 : 0;
 }
 