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
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cmath>

#include <json/json.h>

#include "VideoLayoutConfig.hpp"

using namespace fg;


// SearchRegion implementation

SearchRegion::SearchRegion()
  : x(0)
  , y(0)
  , width(0)
  , height(0)
{
}


SearchRegion::SearchRegion(int x_, int y_, int w, int h)
  : x(x_)
  , y(y_)
  , width(w)
  , height(h)
{
}


// search_region_from_quadrant: 1=top-left, 2=top-right, 3=bottom-right, 4=bottom-left

namespace fg {

SearchRegion search_region_from_quadrant(int frame_width, int frame_height, int quadrant)
{
  int w = frame_width / 2;
  int h = frame_height / 2;
  switch (quadrant) {
    case 1:
      return SearchRegion(0, 0, w, h);
    case 2:
      return SearchRegion(frame_width / 2, 0, w, h);
    case 3:
      return SearchRegion(frame_width / 2, frame_height / 2, w, h);
    case 4:
      return SearchRegion(0, frame_height / 2, w, h);
    default:
      return SearchRegion(0, 0, w, h);
  }
}

} // namespace fg


// DetectionConfig implementation

DetectionConfig::DetectionConfig()
  : search_quadrant(0)
  , match_threshold(0.7)
  , replacement_scale(1.0)
  , conflict_priority(0)
  , full_screen(false)
  , suppress_during_full_screen(false)
{
}


// ReplacementConfig implementation

ReplacementConfig::ReplacementConfig()
  : x(0)
  , y(0)
  , width(100)
  , height(100)
  , scale(1.0)
{
}


int ReplacementConfig::get_scaled_width() const
{
  return static_cast<int>(std::round(width * scale));
}


int ReplacementConfig::get_scaled_height() const
{
  return static_cast<int>(std::round(height * scale));
}


// Resolution implementation

Resolution::Resolution()
  : width(1920)
  , height(1080)
{
}


Resolution::Resolution(int w, int h)
  : width(w)
  , height(h)
{
}


bool Resolution::matches(int w, int h) const
{
  return width == w && height == h;
}


bool Resolution::operator==(const Resolution& other) const
{
  return width == other.width && height == other.height;
}


// VideoLayout implementation

VideoLayout::VideoLayout()
  : resolution(1920, 1080)
{
}


// VideoLayoutManager implementation

VideoLayoutManager::VideoLayoutManager()
  : images_directory_("overlay-images")
  , reference_directory_("reference_images")
  , detection_enabled_(false)
{
}


bool VideoLayoutManager::load_from_file(const std::string& file_path)
{
  std::ifstream file(file_path);
  if (!file.is_open()) {
    return false;
  }
  
  config_file_path_ = file_path;
  
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  
  if (!Json::parseFromStream(builder, file, &root, &errors)) {
    return false;
  }
  
  // Get images directory
  if (root.isMember("images_dir")) {
    images_directory_ = root["images_dir"].asString();
  }
  
  // Get reference images directory
  if (root.isMember("reference_dir")) {
    reference_directory_ = root["reference_dir"].asString();
  }
  
  // Check if detection is enabled
  if (root.isMember("detection_enabled")) {
    detection_enabled_ = root["detection_enabled"].asBool();
  }
  
  // Parse layouts
  layouts_.clear();
  if (root.isMember("layouts") && root["layouts"].isArray()) {
    for (const auto& layout_json : root["layouts"]) {
      layouts_.push_back(parse_layout(layout_json));
    }
  }
  
  rebuild_index();
  return true;
}


bool VideoLayoutManager::save_to_file(const std::string& file_path) const
{
  Json::Value root;
  root["images_dir"] = images_directory_;
  
  Json::Value layouts_array(Json::arrayValue);
  for (const auto& layout : layouts_) {
    Json::Value layout_json;
    layout_json["id"] = layout.id;
    layout_json["name"] = layout.name;
    
    Json::Value resolution_json;
    resolution_json["width"] = layout.resolution.width;
    resolution_json["height"] = layout.resolution.height;
    layout_json["resolution"] = resolution_json;
    
    Json::Value replacements_array(Json::arrayValue);
    for (const auto& repl : layout.replacements) {
      Json::Value repl_json;
      repl_json["name"] = repl.name;
      repl_json["replacement_image"] = repl.replacement_image;
      
      Json::Value pos_json;
      pos_json["x"] = repl.x;
      pos_json["y"] = repl.y;
      repl_json["position"] = pos_json;
      
      Json::Value size_json;
      size_json["width"] = repl.width;
      size_json["height"] = repl.height;
      repl_json["size"] = size_json;
      
      repl_json["scale"] = repl.scale;
      
      replacements_array.append(repl_json);
    }
    layout_json["replacements"] = replacements_array;
    
    layouts_array.append(layout_json);
  }
  root["layouts"] = layouts_array;
  
  std::ofstream file(file_path);
  if (!file.is_open()) {
    return false;
  }
  
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  std::unique_ptr<Json::StreamWriter> json_writer(writer.newStreamWriter());
  json_writer->write(root, &file);
  
  return true;
}


layout_ptr VideoLayoutManager::get_layout(const std::string& id) const
{
  auto it = id_to_index_.find(id);
  if (it != id_to_index_.end()) {
    return std::make_shared<VideoLayout>(layouts_[it->second]);
  }
  return nullptr;
}


layout_ptr VideoLayoutManager::get_layout_for_resolution(int width, int height) const
{
  for (const auto& layout : layouts_) {
    if (layout.resolution.matches(width, height)) {
      return std::make_shared<VideoLayout>(layout);
    }
  }
  return nullptr;
}


const std::vector<VideoLayout>& VideoLayoutManager::get_all_layouts() const
{
  return layouts_;
}


std::vector<std::string> VideoLayoutManager::get_layout_ids() const
{
  std::vector<std::string> ids;
  ids.reserve(layouts_.size());
  for (const auto& layout : layouts_) {
    ids.push_back(layout.id);
  }
  return ids;
}


bool VideoLayoutManager::has_layout(const std::string& id) const
{
  return id_to_index_.find(id) != id_to_index_.end();
}


void VideoLayoutManager::set_images_directory(const std::string& dir)
{
  images_directory_ = dir;
}


std::string VideoLayoutManager::get_images_directory() const
{
  return images_directory_;
}


std::string VideoLayoutManager::get_image_path(const std::string& image_filename) const
{
  if (images_directory_.empty()) {
    return image_filename;
  }
  return images_directory_ + "/" + image_filename;
}


void VideoLayoutManager::set_reference_directory(const std::string& dir)
{
  reference_directory_ = dir;
}


std::string VideoLayoutManager::get_reference_directory() const
{
  return reference_directory_;
}


std::string VideoLayoutManager::get_reference_path(const std::string& image_filename) const
{
  if (reference_directory_.empty()) {
    return image_filename;
  }
  return reference_directory_ + "/" + image_filename;
}


void VideoLayoutManager::rebuild_index()
{
  id_to_index_.clear();
  for (size_t i = 0; i < layouts_.size(); ++i) {
    id_to_index_[layouts_[i].id] = i;
  }
}


VideoLayout VideoLayoutManager::parse_layout(const Json::Value& json) const
{
  VideoLayout layout;
  
  if (json.isMember("id")) {
    layout.id = json["id"].asString();
  }
  
  if (json.isMember("name")) {
    layout.name = json["name"].asString();
  }
  
  if (json.isMember("resolution")) {
    const auto& res = json["resolution"];
    if (res.isMember("width")) {
      layout.resolution.width = res["width"].asInt();
    }
    if (res.isMember("height")) {
      layout.resolution.height = res["height"].asInt();
    }
  }
  
  // Parse old-style replacements (position-based)
  if (json.isMember("replacements") && json["replacements"].isArray()) {
    for (const auto& repl_json : json["replacements"]) {
      layout.replacements.push_back(parse_replacement(repl_json));
    }
  }
  
  // Parse new-style detections (template matching)
  if (json.isMember("detections") && json["detections"].isArray()) {
    for (const auto& det_json : json["detections"]) {
      layout.detections.push_back(parse_detection(det_json));
    }
  }
  
  return layout;
}


ReplacementConfig VideoLayoutManager::parse_replacement(const Json::Value& json) const
{
  ReplacementConfig repl;
  
  if (json.isMember("name")) {
    repl.name = json["name"].asString();
  }
  
  if (json.isMember("replacement_image")) {
    repl.replacement_image = json["replacement_image"].asString();
  }
  
  if (json.isMember("position")) {
    const auto& pos = json["position"];
    if (pos.isMember("x")) {
      repl.x = pos["x"].asInt();
    }
    if (pos.isMember("y")) {
      repl.y = pos["y"].asInt();
    }
  }
  
  if (json.isMember("size")) {
    const auto& size = json["size"];
    if (size.isMember("width")) {
      repl.width = size["width"].asInt();
    }
    if (size.isMember("height")) {
      repl.height = size["height"].asInt();
    }
  }
  
  if (json.isMember("scale")) {
    repl.scale = json["scale"].asDouble();
  }
  
  return repl;
}


DetectionConfig VideoLayoutManager::parse_detection(const Json::Value& json) const
{
  DetectionConfig det;
  
  if (json.isMember("name")) {
    det.name = json["name"].asString();
  }
  
  if (json.isMember("reference_image")) {
    det.reference_image = json["reference_image"].asString();
  }
  
  if (json.isMember("replacement_image")) {
    det.replacement_image = json["replacement_image"].asString();
  }
  
  if (json.isMember("search_region")) {
    const auto& region = json["search_region"];
    if (region.isMember("x")) {
      det.search_region.x = region["x"].asInt();
    }
    if (region.isMember("y")) {
      det.search_region.y = region["y"].asInt();
    }
    if (region.isMember("width")) {
      det.search_region.width = region["width"].asInt();
    }
    if (region.isMember("height")) {
      det.search_region.height = region["height"].asInt();
    }
  }
  
  if (json.isMember("search_quadrant")) {
    det.search_quadrant = json["search_quadrant"].asInt();
  }
  
  if (json.isMember("match_threshold")) {
    det.match_threshold = json["match_threshold"].asDouble();
  }
  
  if (json.isMember("replacement_scale")) {
    det.replacement_scale = json["replacement_scale"].asDouble();
  }

  if (json.isMember("conflict_priority")) {
    det.conflict_priority = json["conflict_priority"].asInt();
  }

  // Optional flags for full-screen suppression logic
  if (json.isMember("full_screen")) {
    det.full_screen = json["full_screen"].asBool();
  }

  if (json.isMember("suppress_during_full_screen")) {
    det.suppress_during_full_screen = json["suppress_during_full_screen"].asBool();
  }
  
  return det;
}

