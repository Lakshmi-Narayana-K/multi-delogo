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
#include <cstdlib>
#include <iostream>
#include <fstream>
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


// Template matching to detect logo in a frame
bool detect_logo_in_frame(const cv::Mat& frame, 
                          const cv::Mat& template_img,
                          const fg::SearchRegion& search_region,
                          double threshold,
                          int& found_x, int& found_y,
                          double& confidence)
{
  // Extract the search region from the frame
  int roi_x = std::max(0, search_region.x);
  int roi_y = std::max(0, search_region.y);
  int roi_w = std::min(search_region.width, frame.cols - roi_x);
  int roi_h = std::min(search_region.height, frame.rows - roi_y);
  
  if (roi_w <= template_img.cols || roi_h <= template_img.rows) {
    return false;
  }
  
  cv::Rect roi(roi_x, roi_y, roi_w, roi_h);
  cv::Mat search_area = frame(roi);
  
  // Perform template matching
  cv::Mat result;
  cv::matchTemplate(search_area, template_img, result, cv::TM_CCOEFF_NORMED);
  
  // Find the best match
  double min_val, max_val;
  cv::Point min_loc, max_loc;
  cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);
  
  confidence = max_val;
  
  if (max_val >= threshold) {
    // Found! Calculate position in full frame
    found_x = roi_x + max_loc.x;
    found_y = roi_y + max_loc.y;
    return true;
  }
  
  return false;
}


// Detect logo presence throughout the video by sampling frames
std::vector<DetectionResult> detect_logos_in_video(
    const std::string& video_path,
    fg::VideoLayoutManager& layout_manager,
    const fg::VideoLayout& layout,
    int sample_interval)
{
  std::vector<DetectionResult> results;
  
  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) {
    std::cerr << "  Error: Cannot open video for detection" << std::endl;
    return results;
  }
  
  int frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  
  // For each detection config in the layout
  for (const auto& det : layout.detections) {
    std::cout << "  Detecting: " << det.name << std::endl;
    
    // Load reference template image
    std::string ref_path = layout_manager.get_reference_path(det.reference_image);
    cv::Mat template_img = cv::imread(ref_path);
    
    if (template_img.empty()) {
      std::cerr << "    Error: Cannot load reference image: " << ref_path << std::endl;
      continue;
    }
    
    std::cout << "    Reference image: " << ref_path << " (" << template_img.cols << "x" << template_img.rows << ")" << std::endl;
    std::cout << "    Search region: (" << det.search_region.x << "," << det.search_region.y 
              << ") " << det.search_region.width << "x" << det.search_region.height << std::endl;
    
    // Track detection state
    bool currently_detecting = false;
    int detection_start_frame = -1;
    int last_detected_x = 0, last_detected_y = 0;
    int consecutive_misses = 0;
    const int MAX_CONSECUTIVE_MISSES = 3;  // Allow some frames without detection
    
    // Sample frames throughout the video
    for (int frame_num = 0; frame_num < frame_count; frame_num += sample_interval) {
      cap.set(cv::CAP_PROP_POS_FRAMES, frame_num);
      
      cv::Mat frame;
      if (!cap.read(frame)) {
        break;
      }
      
      int found_x, found_y;
      double confidence;
      bool found = detect_logo_in_frame(frame, template_img, det.search_region,
                                        det.match_threshold, found_x, found_y, confidence);
      
      if (found) {
        if (!currently_detecting) {
          // Start of a new detection segment
          currently_detecting = true;
          detection_start_frame = frame_num;
          std::cout << "    Logo FOUND at frame " << frame_num 
                    << " pos(" << found_x << "," << found_y << ")"
                    << " confidence: " << (confidence * 100) << "%" << std::endl;
        }
        last_detected_x = found_x;
        last_detected_y = found_y;
        consecutive_misses = 0;
      } else {
        if (currently_detecting) {
          consecutive_misses++;
          if (consecutive_misses >= MAX_CONSECUTIVE_MISSES) {
            // End of detection segment
            int end_frame = frame_num - (consecutive_misses * sample_interval);
            
            DetectionResult result;
            result.found = true;
            result.x = last_detected_x;
            result.y = last_detected_y;
            result.width = template_img.cols;
            result.height = template_img.rows;
            result.confidence = confidence;
            result.start_frame = detection_start_frame;
            result.end_frame = end_frame;
            result.replacement_image = det.replacement_image;
            result.scale = det.replacement_scale;
            
            results.push_back(result);
            
            std::cout << "    Logo segment: frames " << detection_start_frame << "-" << end_frame << std::endl;
            
            currently_detecting = false;
            consecutive_misses = 0;
          }
        }
      }
    }
    
    // Handle case where logo is detected until end of video
    if (currently_detecting) {
      DetectionResult result;
      result.found = true;
      result.x = last_detected_x;
      result.y = last_detected_y;
      result.width = template_img.cols;
      result.height = template_img.rows;
      result.confidence = 0.0;
      result.start_frame = detection_start_frame;
      result.end_frame = frame_count - 1;
      result.replacement_image = det.replacement_image;
      result.scale = det.replacement_scale;
      
      results.push_back(result);
      
      std::cout << "    Logo segment: frames " << detection_start_frame << "-" << (frame_count - 1) << " (end of video)" << std::endl;
    }
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
                   bool dry_run)
{
  std::cout << "\nProcessing: " << input_path << std::endl;
  
  // Get video info
  int width, height, frame_count;
  double fps;
  if (!get_video_info(input_path, width, height, frame_count, fps)) {
    std::cerr << "  Error: Cannot open video file" << std::endl;
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
      return false;
    }
    std::cout << "  Auto-detected layout: " << layout->name << std::endl;
  } else {
    layout = layout_manager.get_layout(layout_id);
    if (!layout) {
      std::cerr << "  Error: Layout '" << layout_id << "' not found" << std::endl;
      return false;
    }
    std::cout << "  Using layout: " << layout->name << std::endl;
  }
  
  // Check if detection is enabled and layout has detections
  std::vector<DetectionResult> detections;
  
  if (layout_manager.is_detection_enabled() && layout->uses_detection()) {
    std::cout << "  Running template matching detection..." << std::endl;
    detections = detect_logos_in_video(input_path, layout_manager, *layout, sample_interval);
    
    if (detections.empty()) {
      std::cout << "  No logos detected in video, skipping" << std::endl;
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
      detections.push_back(result);
    }
  } else {
    std::cout << "  No detections or replacements configured" << std::endl;
    return true;
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
    return true;
  }
  
  // Execute FFmpeg
  std::cout << "\n  Running FFmpeg..." << std::endl;
  int result = system(cmd.str().c_str());
  
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
  
  for (const auto& video_path : videos) {
    std::string output_path = get_output_filename(video_path, output_folder);
    
    if (process_video(video_path, output_path, layout_manager,
                      layout_id, auto_detect, ffmpeg_path, sample_interval, dry_run)) {
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
