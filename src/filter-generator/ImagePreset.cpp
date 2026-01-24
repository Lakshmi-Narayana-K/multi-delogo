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
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stdexcept>

#include <boost/algorithm/string.hpp>

#include "ImagePreset.hpp"

using namespace fg;


ImagePreset::ImagePreset()
  : position_type(PositionType::MARGINS)
  , x(0), y(0), width(0), height(0)
  , margin_left(0), margin_top(0), margin_right(0), margin_bottom(0)
{
}


void ImagePreset::calculate_absolute_position(int frame_width, int frame_height)
{
  switch (position_type) {
  case PositionType::ABSOLUTE:
    // x, y, width, height are already set
    break;
    
  case PositionType::MARGINS:
    // Position from margins, but use explicit width/height
    x = margin_left;
    y = margin_top;
    // width and height are already set explicitly
    break;
    
  case PositionType::MARGINS_FILL:
    // Calculate position and size from all four margins
    x = margin_left;
    y = margin_top;
    width = frame_width - margin_left - margin_right;
    height = frame_height - margin_top - margin_bottom;
    
    // Ensure valid dimensions
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    break;
  }
}


std::string ImagePreset::to_string() const
{
  std::ostringstream ss;
  ss << id << "|" << name << "|" << image_path << "|";
  
  switch (position_type) {
  case PositionType::ABSOLUTE:
    ss << "ABS|" << x << "|" << y << "|" << width << "|" << height;
    break;
    
  case PositionType::MARGINS:
    // Format: MARGIN|left|top|width|height
    ss << "MARGIN|" << margin_left << "|" << margin_top << "|" << width << "|" << height;
    break;
    
  case PositionType::MARGINS_FILL:
    // Format: FILL|left|top|right|bottom
    ss << "FILL|" << margin_left << "|" << margin_top << "|" << margin_right << "|" << margin_bottom;
    break;
  }
  
  return ss.str();
}


ImagePreset ImagePreset::from_string(const std::string& str)
{
  ImagePreset preset;
  
  std::vector<std::string> parts;
  boost::split(parts, str, boost::is_any_of("|"));
  
  if (parts.size() < 8) {
    throw std::invalid_argument("Invalid preset format");
  }
  
  preset.id = parts[0];
  preset.name = parts[1];
  preset.image_path = parts[2];
  
  if (parts[3] == "ABS") {
    preset.position_type = PositionType::ABSOLUTE;
    preset.x = std::stoi(parts[4]);
    preset.y = std::stoi(parts[5]);
    preset.width = std::stoi(parts[6]);
    preset.height = std::stoi(parts[7]);
  } else if (parts[3] == "MARGIN") {
    preset.position_type = PositionType::MARGINS;
    preset.margin_left = std::stoi(parts[4]);
    preset.margin_top = std::stoi(parts[5]);
    preset.width = std::stoi(parts[6]);
    preset.height = std::stoi(parts[7]);
  } else if (parts[3] == "FILL") {
    preset.position_type = PositionType::MARGINS_FILL;
    preset.margin_left = std::stoi(parts[4]);
    preset.margin_top = std::stoi(parts[5]);
    preset.margin_right = std::stoi(parts[6]);
    preset.margin_bottom = std::stoi(parts[7]);
  } else {
    throw std::invalid_argument("Unknown position type: " + parts[3]);
  }
  
  return preset;
}


// ImagePresetManager implementation

ImagePresetManager::ImagePresetManager()
{
}


bool ImagePresetManager::load_from_file(const std::string& file_path)
{
  std::ifstream file(file_path);
  if (!file.is_open()) {
    return false;
  }
  
  // Peek at first non-whitespace character to detect format
  char first_char = 0;
  while (file.get(first_char)) {
    if (!std::isspace(first_char)) {
      break;
    }
  }
  file.close();
  
  // If starts with '{' or '[', it's JSON
  if (first_char == '{' || first_char == '[') {
    return load_from_json(file_path);
  } else {
    return load_from_legacy(file_path);
  }
}


bool ImagePresetManager::load_from_json(const std::string& file_path)
{
  std::ifstream file(file_path);
  if (!file.is_open()) {
    return false;
  }
  
  presets_.clear();
  id_to_index_.clear();
  
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
  
  // Get presets array
  if (root.isMember("presets") && root["presets"].isArray()) {
    for (const auto& preset_json : root["presets"]) {
      try {
        ImagePreset preset = parse_json_preset(preset_json);
        
        // If image path is relative and we have an images directory, make it absolute
        if (!images_directory_.empty() && 
            !preset.image_path.empty() && 
            preset.image_path[0] != '/') {
          preset.image_path = images_directory_ + "/" + preset.image_path;
        }
        
        presets_.push_back(preset);
      } catch (...) {
        // Skip invalid presets
        continue;
      }
    }
  }
  
  rebuild_index();
  return true;
}


bool ImagePresetManager::load_from_legacy(const std::string& file_path)
{
  std::ifstream file(file_path);
  if (!file.is_open()) {
    return false;
  }
  
  presets_.clear();
  id_to_index_.clear();
  
  std::string line;
  while (std::getline(file, line)) {
    // Skip empty lines and comments
    boost::trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    
    // Check for images_directory directive
    if (boost::starts_with(line, "images_dir=")) {
      images_directory_ = line.substr(11);
      continue;
    }
    
    try {
      parse_legacy_line(line);
    } catch (...) {
      // Skip invalid lines
      continue;
    }
  }
  
  rebuild_index();
  return true;
}


ImagePreset ImagePresetManager::parse_json_preset(const Json::Value& json) const
{
  ImagePreset preset;
  
  preset.id = json["id"].asString();
  preset.name = json.get("name", preset.id).asString();
  preset.image_path = json["image"].asString();
  
  // Parse position
  if (json.isMember("position")) {
    const auto& pos = json["position"];
    std::string type = pos.get("type", "absolute").asString();
    
    if (type == "absolute" || type == "abs") {
      preset.position_type = PositionType::ABSOLUTE;
      preset.x = pos["x"].asInt();
      preset.y = pos["y"].asInt();
      preset.width = pos["width"].asInt();
      preset.height = pos["height"].asInt();
    } else if (type == "margin") {
      preset.position_type = PositionType::MARGINS;
      preset.margin_left = pos["left"].asInt();
      preset.margin_top = pos["top"].asInt();
      preset.width = pos["width"].asInt();
      preset.height = pos["height"].asInt();
    } else if (type == "fill") {
      preset.position_type = PositionType::MARGINS_FILL;
      preset.margin_left = pos["left"].asInt();
      preset.margin_top = pos["top"].asInt();
      preset.margin_right = pos["right"].asInt();
      preset.margin_bottom = pos["bottom"].asInt();
    }
  } else {
    // Shorthand: direct x, y, width, height at root level
    preset.position_type = PositionType::ABSOLUTE;
    preset.x = json.get("x", 0).asInt();
    preset.y = json.get("y", 0).asInt();
    preset.width = json.get("width", 0).asInt();
    preset.height = json.get("height", 0).asInt();
  }
  
  return preset;
}


Json::Value ImagePresetManager::preset_to_json(const ImagePreset& preset) const
{
  Json::Value json;
  
  json["id"] = preset.id;
  json["name"] = preset.name;
  json["image"] = preset.image_path;
  
  Json::Value pos;
  switch (preset.position_type) {
  case PositionType::ABSOLUTE:
    pos["type"] = "absolute";
    pos["x"] = preset.x;
    pos["y"] = preset.y;
    pos["width"] = preset.width;
    pos["height"] = preset.height;
    break;
    
  case PositionType::MARGINS:
    pos["type"] = "margin";
    pos["left"] = preset.margin_left;
    pos["top"] = preset.margin_top;
    pos["width"] = preset.width;
    pos["height"] = preset.height;
    break;
    
  case PositionType::MARGINS_FILL:
    pos["type"] = "fill";
    pos["left"] = preset.margin_left;
    pos["top"] = preset.margin_top;
    pos["right"] = preset.margin_right;
    pos["bottom"] = preset.margin_bottom;
    break;
  }
  
  json["position"] = pos;
  return json;
}


bool ImagePresetManager::save_to_file(const std::string& file_path) const
{
  std::ofstream file(file_path);
  if (!file.is_open()) {
    return false;
  }
  
  Json::Value root;
  
  if (!images_directory_.empty()) {
    root["images_dir"] = images_directory_;
  }
  
  Json::Value presets_array(Json::arrayValue);
  for (const auto& preset : presets_) {
    presets_array.append(preset_to_json(preset));
  }
  root["presets"] = presets_array;
  
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
  writer->write(root, &file);
  
  return true;
}


void ImagePresetManager::add_preset(const ImagePreset& preset)
{
  // Check if preset with same ID exists
  auto it = id_to_index_.find(preset.id);
  if (it != id_to_index_.end()) {
    // Update existing preset
    presets_[it->second] = preset;
  } else {
    // Add new preset
    presets_.push_back(preset);
    rebuild_index();
  }
}


void ImagePresetManager::remove_preset(const std::string& id)
{
  auto it = id_to_index_.find(id);
  if (it != id_to_index_.end()) {
    presets_.erase(presets_.begin() + it->second);
    rebuild_index();
  }
}


preset_ptr ImagePresetManager::get_preset(const std::string& id) const
{
  auto it = id_to_index_.find(id);
  if (it != id_to_index_.end()) {
    return std::make_shared<ImagePreset>(presets_[it->second]);
  }
  return nullptr;
}


const std::vector<ImagePreset>& ImagePresetManager::get_all_presets() const
{
  return presets_;
}


std::vector<std::string> ImagePresetManager::get_preset_ids() const
{
  std::vector<std::string> ids;
  for (const auto& preset : presets_) {
    ids.push_back(preset.id);
  }
  return ids;
}


bool ImagePresetManager::has_preset(const std::string& id) const
{
  return id_to_index_.find(id) != id_to_index_.end();
}


std::string ImagePresetManager::get_default_config_path()
{
  // Default to current directory
  return "image_presets.conf";
}


void ImagePresetManager::set_images_directory(const std::string& dir)
{
  images_directory_ = dir;
}


std::string ImagePresetManager::get_images_directory() const
{
  return images_directory_;
}


void ImagePresetManager::rebuild_index()
{
  id_to_index_.clear();
  for (size_t i = 0; i < presets_.size(); ++i) {
    id_to_index_[presets_[i].id] = i;
  }
}


void ImagePresetManager::parse_legacy_line(const std::string& line)
{
  ImagePreset preset = ImagePreset::from_string(line);
  
  // If image path is relative and we have an images directory, make it absolute
  if (!images_directory_.empty() && 
      !preset.image_path.empty() && 
      preset.image_path[0] != '/') {
    preset.image_path = images_directory_ + "/" + preset.image_path;
  }
  
  presets_.push_back(preset);
}

