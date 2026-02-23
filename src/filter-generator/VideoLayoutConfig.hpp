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
 #ifndef FG_VIDEO_LAYOUT_CONFIG_H
 #define FG_VIDEO_LAYOUT_CONFIG_H
 
 #include <string>
 #include <vector>
 #include <map>
 #include <memory>
 
 #include <json/json.h>
 
 
 namespace fg {
 
   /**
    * Search region for template matching - limits where to look for logo
    */
   struct SearchRegion {
     int x;
     int y;
     int width;
     int height;
     
     SearchRegion();
     SearchRegion(int x, int y, int w, int h);
   };
 
   /**
    * Compute search region for a quadrant of the frame.
    * Quadrant: 1=top-left, 2=top-right, 3=bottom-right, 4=bottom-left.
    * If quadrant is not 1-4, returns quadrant 1 (top-left).
    */
   SearchRegion search_region_from_quadrant(int frame_width, int frame_height, int quadrant);
 
   /**
    * Configuration for detecting and replacing a logo using template matching.
    */
   struct DetectionConfig {
     std::string name;              // Display name (e.g., "Top Right Logo")
     std::string reference_image;   // Image to search for (template)
     std::string replacement_image; // Image to overlay when found
     SearchRegion search_region;     // Where to look in the frame (used when search_quadrant is 0)
     int search_quadrant;           // 0=use search_region, 1-4=use quadrant (1=TL, 2=TR, 3=BR, 4=BL)
     double match_threshold;        // Confidence threshold (0.0-1.0)
     double replacement_scale;      // Scale factor for replacement
     int conflict_priority;         // When two logos are detected at same position, higher priority wins (default 0; ties broken by confidence)
     bool full_screen;              // When true, this segment defines "full-screen" ranges; other overlays can be suppressed during it
     bool suppress_during_full_screen;  // When true, do not draw this overlay during any full_screen segment's frame range
     
     DetectionConfig();
   };
 
   /**
    * Configuration for a single logo replacement within a layout.
    * Specifies where to place a replacement image and how to scale it.
    */
   struct ReplacementConfig {
     std::string name;              // Display name (e.g., "Top Right Logo")
     std::string replacement_image; // Image filename to overlay
     
     // Position in pixels (top-left corner of overlay)
     int x;
     int y;
     
     // Size of the area to cover
     int width;
     int height;
     
     // Scale factor for the replacement image
     // 1.0 = original size, 0.5 = half, 2.0 = double
     double scale;
     
     ReplacementConfig();
     
     // Get the scaled dimensions for FFmpeg
     int get_scaled_width() const;
     int get_scaled_height() const;
   };
 
   /**
    * Video resolution for layout matching
    */
   struct Resolution {
     int width;
     int height;
     
     Resolution();
     Resolution(int w, int h);
     
     bool matches(int w, int h) const;
     bool operator==(const Resolution& other) const;
   };
 
   /**
    * A video layout defines a set of logo positions for a specific video type.
    * For example, "Speaker Full Screen" might have a logo at (1733, 55).
    */
   struct VideoLayout {
     std::string id;         // Unique identifier (e.g., "speaker_fullscreen")
     std::string name;       // Display name (e.g., "Speaker Full Screen")
     Resolution resolution;  // Video resolution this layout applies to
     
     // Old-style position-based replacements (for backward compatibility)
     std::vector<ReplacementConfig> replacements;
     
     // New-style template matching detections
     std::vector<DetectionConfig> detections;
     
     VideoLayout();
     
     // Check if this layout uses detection or position-based replacement
     bool uses_detection() const { return !detections.empty(); }
   };
 
   typedef std::shared_ptr<VideoLayout> layout_ptr;
 
 
   /**
    * Manager for video layout configurations.
    * Loads layouts from JSON and provides lookup functionality.
    */
   class VideoLayoutManager {
   public:
     VideoLayoutManager();
     
     // Load layouts from a JSON configuration file
     bool load_from_file(const std::string& file_path);
     
     // Save layouts to a JSON file
     bool save_to_file(const std::string& file_path) const;
     
     // Get layout by ID
     layout_ptr get_layout(const std::string& id) const;
     
     // Get layout by resolution (returns first match)
     layout_ptr get_layout_for_resolution(int width, int height) const;
     
     // Get all layouts
     const std::vector<VideoLayout>& get_all_layouts() const;
     
     // Get all layout IDs
     std::vector<std::string> get_layout_ids() const;
     
     // Check if layout exists
     bool has_layout(const std::string& id) const;
     
     // Get the images base directory
     void set_images_directory(const std::string& dir);
     std::string get_images_directory() const;
     
     // Get the reference images directory
     void set_reference_directory(const std::string& dir);
     std::string get_reference_directory() const;
     
     // Get full path to a replacement image
     std::string get_image_path(const std::string& image_filename) const;
     
     // Get full path to a reference image
     std::string get_reference_path(const std::string& image_filename) const;
     
     // Check if detection is enabled
     bool is_detection_enabled() const { return detection_enabled_; }
     
     // Get config file path
     const std::string& get_config_file_path() const { return config_file_path_; }
     
   private:
     std::vector<VideoLayout> layouts_;
     std::map<std::string, size_t> id_to_index_;
     std::string images_directory_;
     std::string reference_directory_;
     std::string config_file_path_;
     bool detection_enabled_;
     
     void rebuild_index();
     VideoLayout parse_layout(const Json::Value& json) const;
     ReplacementConfig parse_replacement(const Json::Value& json) const;
     DetectionConfig parse_detection(const Json::Value& json) const;
   };
 
 }
 
 #endif // FG_VIDEO_LAYOUT_CONFIG_H
 
 