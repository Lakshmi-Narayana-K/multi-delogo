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
#ifndef FG_IMAGE_PRESET_H
#define FG_IMAGE_PRESET_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <json/json.h>


namespace fg {

  // Position can be defined as:
  // - ABSOLUTE: x, y, width, height (exact pixel positions)
  // - MARGINS: left, top distance from edges + explicit width, height
  // - MARGINS_FILL: left, top, right, bottom (fills remaining space)
  enum class PositionType {
    ABSOLUTE,      // x, y, width, height
    MARGINS,       // margin_left, margin_top + width, height
    MARGINS_FILL   // margin_left, margin_top, margin_right, margin_bottom (calculated size)
  };

  struct ImagePreset {
    std::string id;           // Unique identifier (e.g., "logo1", "watermark")
    std::string name;         // Display name
    std::string image_path;   // Path to the image file
    
    PositionType position_type;
    
    // For ABSOLUTE positioning: exact x, y, width, height
    // For MARGINS positioning: calculated x, y from margins, explicit width, height
    // For MARGINS_FILL: calculated x, y, width, height from margins
    int x;
    int y;
    int width;
    int height;
    
    // For MARGINS and MARGINS_FILL positioning (distance from frame edges)
    int margin_left;
    int margin_top;
    int margin_right;   // Only used for MARGINS_FILL
    int margin_bottom;  // Only used for MARGINS_FILL
    
    ImagePreset();
    
    // Calculate absolute position from margins given frame dimensions
    void calculate_absolute_position(int frame_width, int frame_height);
    
    // Save/load string representation
    std::string to_string() const;
    static ImagePreset from_string(const std::string& str);
  };

  typedef std::shared_ptr<ImagePreset> preset_ptr;


  class ImagePresetManager {
  public:
    ImagePresetManager();
    
    // Load presets from a configuration file (auto-detects JSON or legacy format)
    bool load_from_file(const std::string& file_path);
    
    // Save presets to a JSON file
    bool save_to_file(const std::string& file_path) const;
    
    // Add a new preset
    void add_preset(const ImagePreset& preset);
    
    // Remove a preset by ID
    void remove_preset(const std::string& id);
    
    // Get preset by ID
    preset_ptr get_preset(const std::string& id) const;
    
    // Get all presets
    const std::vector<ImagePreset>& get_all_presets() const;
    
    // Get all preset IDs
    std::vector<std::string> get_preset_ids() const;
    
    // Check if preset exists
    bool has_preset(const std::string& id) const;
    
    // Get the config file path (relative to project or global)
    static std::string get_default_config_path();
    
    // Set the images base directory
    void set_images_directory(const std::string& dir);
    std::string get_images_directory() const;
    
  private:
    std::vector<ImagePreset> presets_;
    std::map<std::string, size_t> id_to_index_;
    std::string images_directory_;
    
    void rebuild_index();
    
    // JSON loading/saving
    bool load_from_json(const std::string& file_path);
    bool load_from_legacy(const std::string& file_path);
    void parse_legacy_line(const std::string& line);
    ImagePreset parse_json_preset(const Json::Value& json) const;
    Json::Value preset_to_json(const ImagePreset& preset) const;
  };

}

#endif // FG_IMAGE_PRESET_H

