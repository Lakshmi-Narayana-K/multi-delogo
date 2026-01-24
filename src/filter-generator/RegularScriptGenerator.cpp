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
#include <memory>
#include <string>
#include <utility>
#include <ostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <boost/optional.hpp>

#include "RegularScriptGenerator.hpp"
#include "Filters.hpp"
#include "FilterFactory.hpp"
#include "FilterList.hpp"

using namespace fg;


RegularScriptGenerator::RegularScriptGenerator(const FilterList& filter_list,
                                               int frame_width, int frame_height, double fps,
                                               maybe_int scale_width, maybe_int scale_height,
                                               bool no_audio)
  : ScriptGenerator(fps, no_audio)
  , filter_list_(filter_list)
  , frame_width_(frame_width)
  , frame_height_(frame_height)
  , scale_width_(scale_width)
  , scale_height_(scale_height)
  , first_filter_(true)
{
}


std::shared_ptr<RegularScriptGenerator> RegularScriptGenerator::create(const FilterList& filter_list, int frame_width, int frame_height, double fps, maybe_int scale_width, maybe_int scale_height, bool no_audio)
{
  return std::shared_ptr<RegularScriptGenerator>(new RegularScriptGenerator(filter_list, frame_width, frame_height, fps, scale_width, scale_height, no_audio));
}


void RegularScriptGenerator::generate_ffmpeg_script(std::ostream& out) const
{
  if (filter_list_.empty()) {
    return;
  }

  // Check if we have any CUT or SPEED filters (which require segmented processing)
  bool has_complex_filters = false;
  for (const auto& entry : filter_list_) {
    if (entry.filter->type() == FilterType::CUT ||
        entry.filter->type() == FilterType::SPEED) {
      has_complex_filters = true;
      break;
    }
  }

  if (has_complex_filters) {
    // Fall back to segmented processing for CUT/SPEED filters
    generate_segmented_script(out);
  } else {
    // Use single-pass processing with enable expressions for delogo/drawbox
    generate_single_pass_script(out);
  }
}


void RegularScriptGenerator::generate_single_pass_script(std::ostream& out) const
{
  // Collect all visual filters (delogo, drawbox) with their frame ranges
  std::vector<std::string> video_filters;

  // Collect overlay filters separately - they need special handling
  std::vector<std::pair<int, const FilterEntry*>> overlay_filters;  // input_index, entry
  int overlay_input_index = 1;  // Start at 1 since 0 is the main video

  for (const auto& entry : filter_list_) {
    if (entry.filter->type() == FilterType::NO_OP ||
        entry.filter->type() == FilterType::REVIEW) {
      continue;
    }

    if (entry.filter->type() == FilterType::IMAGE_OVERLAY) {
      // Store for later processing
      overlay_filters.push_back(std::make_pair(overlay_input_index++, &entry));
      continue;
    }

    std::string filter_str = entry.filter->ffmpeg_str_with_enable(
      frame_width_, frame_height_,
      entry.start_frame - 1,  // Convert to 0-based frame numbering
      entry.end_frame == NO_END_FRAME ? NO_END_FRAME : entry.end_frame - 1
    );

    if (!filter_str.empty()) {
      video_filters.push_back(filter_str);
    }
  }

  // Start with main video input
  if (video_filters.empty() && overlay_filters.empty()) {
    // No actual filters, just pass through
    out << "[0:v]null";
  } else if (video_filters.empty()) {
    out << "[0:v]null";
  } else {
    // Chain all delogo/drawbox filters together
    out << "[0:v]" << boost::algorithm::join(video_filters, ",");
  }

  // Now handle overlay filters - each needs to be chained
  if (!overlay_filters.empty()) {
    std::string current_output = "[tmp0]";

    // First, close the delogo/drawbox chain
    out << current_output << ";\n";

    // Generate scale filters for each overlay image
    for (size_t i = 0; i < overlay_filters.size(); ++i) {
      int input_idx = overlay_filters[i].first;
      const FilterEntry* entry = overlay_filters[i].second;
      auto overlay = std::dynamic_pointer_cast<ImageOverlayFilter>(entry->filter);

      // Scale the image to the specified dimensions
      out << "[" << input_idx << ":v]scale=" << overlay->width() << ":" << overlay->height()
          << "[img" << i << "];\n";
    }

    // Chain overlay filters
    std::string prev_output = "[tmp0]";
    for (size_t i = 0; i < overlay_filters.size(); ++i) {
      const FilterEntry* entry = overlay_filters[i].second;
      auto overlay = std::dynamic_pointer_cast<ImageOverlayFilter>(entry->filter);

      int start_frame = entry->start_frame - 1;
      int end_frame = entry->end_frame == NO_END_FRAME ? NO_END_FRAME : entry->end_frame - 1;

      // Build enable expression
      std::string enable_expr;
      if (end_frame == NO_END_FRAME) {
        enable_expr = ":enable='gte(n," + std::to_string(start_frame) + ")'";
      } else {
        enable_expr = ":enable='between(n," + std::to_string(start_frame) + "," + std::to_string(end_frame) + ")'";
      }

      std::string next_output = (i == overlay_filters.size() - 1) ? "" : "[tmp" + std::to_string(i + 1) + "]";

      out << prev_output << "[img" << i << "]overlay="
          << overlay->x() << ":" << overlay->y()
          << enable_expr;

      if (!next_output.empty()) {
        out << next_output << ";\n";
        prev_output = next_output;
      }
    }
  }

  // Add scaling if needed
  if (scale_width_) {
    out << ",scale=" << *scale_width_ << ":" << *scale_height_;
  }

  out << "[out_v]";

  // Handle audio
  if (!no_audio_) {
    out << ";\n[0:a]anull[out_a]";
  }
}


std::vector<std::string> RegularScriptGenerator::get_additional_inputs() const
{
  std::vector<std::string> inputs;

  for (const auto& entry : filter_list_) {
    if (entry.filter->type() == FilterType::IMAGE_OVERLAY) {
      auto overlay = std::dynamic_pointer_cast<ImageOverlayFilter>(entry.filter);
      if (!overlay->image_path().empty()) {
        inputs.push_back(overlay->image_path());
      }
    }
  }

  return inputs;
}


void RegularScriptGenerator::generate_segmented_script(std::ostream& out) const
{
  // Original segmented processing for CUT/SPEED filters
  int n_segments = generate_filter_segments(out);
  generate_final_concat(out, n_segments);
}


int RegularScriptGenerator::generate_filter_segments(std::ostream& out) const
{
  int segment = 0;
  FilterList::const_iterator i = filter_list_.begin();
  while (i != filter_list_.end()) {
    const auto& current = *i++;
    filter_ptr filter = current.filter;

    int start_frame = current.start_frame - 1;
    maybe_int next_start_frame;
    if (i != filter_list_.end()) {
      next_start_frame = boost::make_optional(i->start_frame - 1);
    }

    // Use end_frame if specified, otherwise use next filter's start
    maybe_int end_frame;
    if (current.end_frame != NO_END_FRAME) {
      end_frame = boost::make_optional(current.end_frame);
    } else if (next_start_frame) {
      end_frame = next_start_frame;
    }

    if (first_filter_does_not_start_at_first_frame(start_frame)) {
      copy_first_segment_unchanged(out, start_frame);
      segment++;
    }
    first_filter_ = false;

    if (filter->type() == FilterType::CUT) {
      cuts_.push_back(std::make_pair(start_frame, end_frame));
      continue;
    }

    generate_segment(out, segment, filter, start_frame, end_frame);

    ++segment;
  }

  return segment;
}


bool RegularScriptGenerator::first_filter_does_not_start_at_first_frame(int start_frame) const
{
  return first_filter_ && start_frame != 0;
}


void RegularScriptGenerator::copy_first_segment_unchanged(std::ostream& out, int next_start) const
{
  generate_segment(out, 0, FilterFactory::create(FilterType::NO_OP), 0, next_start);
}


void RegularScriptGenerator::generate_segment(std::ostream& out, int segment, filter_ptr filter,
                                              int start_frame, maybe_int next_start_frame) const
{
  std::string ffmpeg_str = filter->ffmpeg_str(frame_width_, frame_height_);
  out << "[0:v]" << generate_trim(start_frame, next_start_frame) << ",setpts=PTS-STARTPTS";
  if (ffmpeg_str != "") {
    out << "," << ffmpeg_str;
  }
  if (scale_width_) {
    out << ",scale=" << *scale_width_ << ":" << *scale_height_;
  }
  out << "[vs" << segment << "];\n";

  if (!no_audio_) {
    std::string ffmpeg_audio_str = filter->ffmpeg_audio_str();
    out << "[0:a]" << generate_atrim(start_frame, next_start_frame)
        << ",asetpts=PTS-STARTPTS";
    if (ffmpeg_audio_str != "") {
      out << "," << ffmpeg_audio_str;
    }
    out << "[as" << segment << "];\n";
  }
}


std::string RegularScriptGenerator::generate_trim(int start_frame, maybe_int next_start_frame) const
{
  if (next_start_frame) {
    return "trim=start_frame=" + std::to_string(start_frame)
      + ":end_frame=" + std::to_string(*next_start_frame);
  } else {
    return "trim=start_frame=" + std::to_string(start_frame);
  }
}


std::string RegularScriptGenerator::generate_atrim(int start_frame, maybe_int next_start_frame) const
{
  std::stringstream out;
  out << std::fixed << std::setprecision(3);
  double start_time = start_frame/fps_;
  if (next_start_frame) {
    double end_time = *next_start_frame/fps_;
    out << "atrim=start=" << start_time << ":end=" << end_time;
  } else {
    out << "atrim=start=" << start_time;
  }
  return out.str();
}


void RegularScriptGenerator::generate_final_concat(std::ostream& out, int n_segments) const
{
  if (!no_audio_) {
    for (int i = 0; i < n_segments; ++i) {
      out << "[vs" << i << "][as" << i << "]";
    }
    out << "concat=n=" << n_segments << ":v=1:a=1[out_v][out_a]";
  } else {
    for (int i = 0; i < n_segments; ++i) {
      out << "[vs" << i << "]";
    }
    out << "concat=n=" << n_segments << ":v=1:a=0[out_v]";
  }
}


int RegularScriptGenerator::resulting_frames(int original_frames) const
{
  int cut_frames = std::accumulate(cuts_.begin(), cuts_.end(), 0,
    [original_frames](int sum, std::pair<int, maybe_int>& i) {
      return sum + (i.second.value_or(original_frames) - i.first);
    });

  return original_frames - cut_frames;
}
