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
#ifndef MDL_AUTO_REPLACE_WINDOW_H
#define MDL_AUTO_REPLACE_WINDOW_H

#include <memory>
#include <string>

#include <gtkmm.h>

#include "filter-generator/FilterData.hpp"
#include "filter-generator/VideoLayoutConfig.hpp"

#include "MultiDelogoAppWindow.hpp"


namespace mdl {
  
  /**
   * Window for auto-replacing logos based on predefined layouts.
   * Allows selecting a layout and applying replacements to the current video,
   * or batch processing a folder of videos.
   */
  class AutoReplaceWindow : public MultiDelogoAppWindow
  {
  public:
    static AutoReplaceWindow* create(fg::FilterData& filter_data,
                                     int total_frames,
                                     int frame_width,
                                     int frame_height,
                                     const std::string& video_file_path);

    AutoReplaceWindow(BaseObjectType* cobject,
                      const Glib::RefPtr<Gtk::Builder>& builder,
                      fg::FilterData& filter_data,
                      int total_frames,
                      int frame_width,
                      int frame_height,
                      const std::string& video_file_path);
    
    ~AutoReplaceWindow();

    // Signal emitted when filters are applied and the main window should refresh
    typedef sigc::signal<void> type_signal_filters_applied;
    type_signal_filters_applied signal_filters_applied();

  private:
    fg::FilterData& filter_data_;
    int total_frames_;
    int frame_width_;
    int frame_height_;
    std::string video_file_path_;
    
    fg::VideoLayoutManager layout_manager_;
    fg::layout_ptr current_layout_;
    
    // UI elements
    Gtk::FileChooserButton* btn_config_file_;
    Gtk::Button* btn_reload_config_;
    Gtk::ComboBoxText* cmb_layout_;
    Gtk::TreeView* tree_replacements_;
    Gtk::Label* lbl_status_;
    Gtk::ProgressBar* progress_bar_;
    Gtk::Button* btn_apply_;
    Gtk::Button* btn_batch_;
    Gtk::Button* btn_close_;
    
    // Replacements tree model
    class ReplacementColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
      ReplacementColumns()
      {
        add(enabled);
        add(name);
        add(position);
        add(size);
        add(image);
        add(scale);
      }
      
      Gtk::TreeModelColumn<bool> enabled;
      Gtk::TreeModelColumn<Glib::ustring> name;
      Gtk::TreeModelColumn<Glib::ustring> position;
      Gtk::TreeModelColumn<Glib::ustring> size;
      Gtk::TreeModelColumn<Glib::ustring> image;
      Gtk::TreeModelColumn<double> scale;
    };
    
    ReplacementColumns replacement_columns_;
    Glib::RefPtr<Gtk::ListStore> replacement_store_;
    
    type_signal_filters_applied signal_filters_applied_;
    
    void configure_widgets(const Glib::RefPtr<Gtk::Builder>& builder);
    void setup_tree_view();
    
    void load_config_file(const std::string& file_path);
    void populate_layouts();
    void update_replacements_list();
    
    void on_config_file_changed();
    void on_reload_config();
    void on_layout_changed();
    void on_apply();
    void on_batch();
    void on_close();
    
    bool on_delete_event(GdkEventAny*) override;
    
    void apply_replacements_to_filter_list();
    void set_status(const std::string& message);
    
    std::string find_default_config_file();
  };

}

#endif // MDL_AUTO_REPLACE_WINDOW_H

