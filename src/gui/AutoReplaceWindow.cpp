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
#include <sstream>

#include <gtkmm.h>
#include <glibmm/i18n.h>

#include "filter-generator/FilterData.hpp"
#include "filter-generator/FilterFactory.hpp"
#include "filter-generator/Filters.hpp"
#include "filter-generator/VideoLayoutConfig.hpp"

#include "AutoReplaceWindow.hpp"
#include "Utils.hpp"

using namespace mdl;


AutoReplaceWindow* AutoReplaceWindow::create(fg::FilterData& filter_data,
                                             int total_frames,
                                             int frame_width,
                                             int frame_height,
                                             const std::string& video_file_path)
{
  auto builder = Gtk::Builder::create_from_resource("/wt/multi-delogo/AutoReplaceWindow.ui");
  AutoReplaceWindow* window = nullptr;
  builder->get_widget_derived("auto_replace_window", window,
                              filter_data, total_frames,
                              frame_width, frame_height,
                              video_file_path);
  return window;
}


AutoReplaceWindow::AutoReplaceWindow(BaseObjectType* cobject,
                                     const Glib::RefPtr<Gtk::Builder>& builder,
                                     fg::FilterData& filter_data,
                                     int total_frames,
                                     int frame_width,
                                     int frame_height,
                                     const std::string& video_file_path)
  : MultiDelogoAppWindow(cobject)
  , filter_data_(filter_data)
  , total_frames_(total_frames)
  , frame_width_(frame_width)
  , frame_height_(frame_height)
  , video_file_path_(video_file_path)
  , btn_config_file_(nullptr)
  , btn_reload_config_(nullptr)
  , cmb_layout_(nullptr)
  , tree_replacements_(nullptr)
  , lbl_status_(nullptr)
  , progress_bar_(nullptr)
  , btn_apply_(nullptr)
  , btn_batch_(nullptr)
  , btn_close_(nullptr)
{
  configure_widgets(builder);
  
  // Try to load default config file
  std::string default_config = find_default_config_file();
  if (!default_config.empty()) {
    load_config_file(default_config);
    btn_config_file_->set_filename(default_config);
  }
}


AutoReplaceWindow::~AutoReplaceWindow()
{
}


void AutoReplaceWindow::configure_widgets(const Glib::RefPtr<Gtk::Builder>& builder)
{
  builder->get_widget("btn_config_file", btn_config_file_);
  builder->get_widget("btn_reload_config", btn_reload_config_);
  builder->get_widget("cmb_layout", cmb_layout_);
  builder->get_widget("tree_replacements", tree_replacements_);
  builder->get_widget("lbl_status", lbl_status_);
  builder->get_widget("progress_bar", progress_bar_);
  builder->get_widget("btn_apply", btn_apply_);
  builder->get_widget("btn_batch", btn_batch_);
  builder->get_widget("btn_close", btn_close_);
  
  // Set up file filter for JSON files
  auto filter = Gtk::FileFilter::create();
  filter->set_name(_("JSON files"));
  filter->add_pattern("*.json");
  btn_config_file_->add_filter(filter);
  
  auto all_filter = Gtk::FileFilter::create();
  all_filter->set_name(_("All files"));
  all_filter->add_pattern("*");
  btn_config_file_->add_filter(all_filter);
  
  // Connect signals
  btn_config_file_->signal_file_set().connect(
    sigc::mem_fun(*this, &AutoReplaceWindow::on_config_file_changed));
  btn_reload_config_->signal_clicked().connect(
    sigc::mem_fun(*this, &AutoReplaceWindow::on_reload_config));
  cmb_layout_->signal_changed().connect(
    sigc::mem_fun(*this, &AutoReplaceWindow::on_layout_changed));
  btn_apply_->signal_clicked().connect(
    sigc::mem_fun(*this, &AutoReplaceWindow::on_apply));
  btn_batch_->signal_clicked().connect(
    sigc::mem_fun(*this, &AutoReplaceWindow::on_batch));
  btn_close_->signal_clicked().connect(
    sigc::mem_fun(*this, &AutoReplaceWindow::on_close));
  
  setup_tree_view();
  
  // Update status
  std::stringstream ss;
  ss << _("Video: ") << frame_width_ << "x" << frame_height_;
  set_status(ss.str());
}


void AutoReplaceWindow::setup_tree_view()
{
  replacement_store_ = Gtk::ListStore::create(replacement_columns_);
  tree_replacements_->set_model(replacement_store_);
  
  // Enabled column with checkbox
  auto toggle_renderer = Gtk::manage(new Gtk::CellRendererToggle());
  toggle_renderer->set_activatable(true);
  toggle_renderer->signal_toggled().connect([this](const Glib::ustring& path) {
    auto iter = replacement_store_->get_iter(path);
    if (iter) {
      (*iter)[replacement_columns_.enabled] = !(*iter)[replacement_columns_.enabled];
    }
  });
  
  auto col_enabled = Gtk::manage(new Gtk::TreeViewColumn(_("Apply"), *toggle_renderer));
  col_enabled->add_attribute(toggle_renderer->property_active(), replacement_columns_.enabled);
  tree_replacements_->append_column(*col_enabled);
  
  // Other columns
  tree_replacements_->append_column(_("Name"), replacement_columns_.name);
  tree_replacements_->append_column(_("Position"), replacement_columns_.position);
  tree_replacements_->append_column(_("Size"), replacement_columns_.size);
  tree_replacements_->append_column(_("Image"), replacement_columns_.image);
  tree_replacements_->append_column(_("Scale"), replacement_columns_.scale);
  
  // Make columns resizable
  for (auto* col : tree_replacements_->get_columns()) {
    col->set_resizable(true);
  }
}


std::string AutoReplaceWindow::find_default_config_file()
{
  // Look for video_layouts.json in:
  // 1. Same directory as video file
  // 2. Project directory
  // 3. Current working directory
  
  std::vector<std::string> search_paths;
  
  // Directory of video file
  if (!video_file_path_.empty()) {
    auto pos = video_file_path_.rfind('/');
    if (pos != std::string::npos) {
      search_paths.push_back(video_file_path_.substr(0, pos) + "/video_layouts.json");
    }
  }
  
  // Current directory
  search_paths.push_back("video_layouts.json");
  
  // Home directory
  const char* home = std::getenv("HOME");
  if (home) {
    search_paths.push_back(std::string(home) + "/video_layouts.json");
  }
  
  for (const auto& path : search_paths) {
    if (Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
      return path;
    }
  }
  
  return "";
}


void AutoReplaceWindow::load_config_file(const std::string& file_path)
{
  if (file_path.empty()) {
    return;
  }
  
  if (!layout_manager_.load_from_file(file_path)) {
    Gtk::MessageDialog dlg(*this,
                           _("Failed to load configuration file"),
                           false, Gtk::MESSAGE_ERROR);
    dlg.set_secondary_text(file_path);
    dlg.run();
    return;
  }
  
  // Update images directory relative to config file
  auto pos = file_path.rfind('/');
  if (pos != std::string::npos) {
    std::string config_dir = file_path.substr(0, pos);
    std::string images_dir = config_dir + "/" + layout_manager_.get_images_directory();
    layout_manager_.set_images_directory(images_dir);
  }
  
  populate_layouts();
  set_status(_("Configuration loaded"));
}


void AutoReplaceWindow::populate_layouts()
{
  cmb_layout_->remove_all();
  
  // Add auto-detect option
  cmb_layout_->append("auto", _("Auto-detect by resolution"));
  
  // Add all layouts
  for (const auto& layout : layout_manager_.get_all_layouts()) {
    std::stringstream label;
    label << layout.name << " (" << layout.resolution.width << "x" << layout.resolution.height << ")";
    cmb_layout_->append(layout.id, label.str());
  }
  
  // Try to auto-detect layout
  auto detected = layout_manager_.get_layout_for_resolution(frame_width_, frame_height_);
  if (detected) {
    cmb_layout_->set_active_id(detected->id);
  } else {
    cmb_layout_->set_active(0);  // Auto-detect
  }
}


void AutoReplaceWindow::update_replacements_list()
{
  replacement_store_->clear();
  
  if (!current_layout_) {
    return;
  }
  
  for (const auto& repl : current_layout_->replacements) {
    auto row = *(replacement_store_->append());
    row[replacement_columns_.enabled] = true;
    row[replacement_columns_.name] = repl.name;
    
    std::stringstream pos;
    pos << repl.x << ", " << repl.y;
    row[replacement_columns_.position] = pos.str();
    
    std::stringstream size;
    size << repl.width << "x" << repl.height;
    row[replacement_columns_.size] = size.str();
    
    row[replacement_columns_.image] = repl.replacement_image;
    row[replacement_columns_.scale] = repl.scale;
  }
}


void AutoReplaceWindow::on_config_file_changed()
{
  std::string file_path = btn_config_file_->get_filename();
  load_config_file(file_path);
}


void AutoReplaceWindow::on_reload_config()
{
  std::string file_path = btn_config_file_->get_filename();
  if (!file_path.empty()) {
    load_config_file(file_path);
  }
}


void AutoReplaceWindow::on_layout_changed()
{
  std::string layout_id = cmb_layout_->get_active_id();
  
  if (layout_id == "auto") {
    // Auto-detect by resolution
    current_layout_ = layout_manager_.get_layout_for_resolution(frame_width_, frame_height_);
    if (!current_layout_) {
      set_status(_("No matching layout found for video resolution"));
    }
  } else {
    current_layout_ = layout_manager_.get_layout(layout_id);
  }
  
  update_replacements_list();
  
  if (current_layout_) {
    std::stringstream ss;
    ss << _("Layout: ") << current_layout_->name 
       << " (" << current_layout_->replacements.size() << _(" replacements)");
    set_status(ss.str());
  }
}


void AutoReplaceWindow::on_apply()
{
  if (!current_layout_) {
    Gtk::MessageDialog dlg(*this,
                           _("No layout selected"),
                           false, Gtk::MESSAGE_WARNING);
    dlg.run();
    return;
  }
  
  apply_replacements_to_filter_list();
  signal_filters_applied_.emit();
  
  std::stringstream ss;
  ss << _("Applied ") << current_layout_->replacements.size() << _(" replacement(s)");
  set_status(ss.str());
  
  Gtk::MessageDialog dlg(*this,
                         _("Replacements applied successfully!"),
                         false, Gtk::MESSAGE_INFO);
  dlg.set_secondary_text(_("The filters have been added. You can now review and encode the video."));
  dlg.run();
}


void AutoReplaceWindow::apply_replacements_to_filter_list()
{
  if (!current_layout_) {
    return;
  }
  
  // Get enabled replacements from the tree view
  auto children = replacement_store_->children();
  int repl_index = 0;
  
  for (auto iter = children.begin(); iter != children.end(); ++iter, ++repl_index) {
    if (!(*iter)[replacement_columns_.enabled]) {
      continue;  // Skip disabled replacements
    }
    
    if (repl_index >= static_cast<int>(current_layout_->replacements.size())) {
      break;
    }
    
    const auto& repl = current_layout_->replacements[repl_index];
    
    // Get the full image path
    std::string image_path = layout_manager_.get_image_path(repl.replacement_image);
    
    // Create an ImageOverlayFilter
    auto filter = fg::FilterFactory::create(
      fg::FilterType::IMAGE_OVERLAY,
      repl.x, repl.y,
      repl.get_scaled_width(), repl.get_scaled_height(),
      image_path
    );
    
    // Add to filter list for entire video (frame 1 to total_frames)
    filter_data_.filter_list().insert(1, total_frames_, filter);
  }
}


void AutoReplaceWindow::on_batch()
{
  // Show folder selection dialog
  Gtk::FileChooserDialog dlg(*this, _("Select Input Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
  dlg.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
  dlg.add_button(_("_Select"), Gtk::RESPONSE_OK);
  
  if (dlg.run() != Gtk::RESPONSE_OK) {
    return;
  }
  
  std::string input_folder = dlg.get_filename();
  
  // Show output folder selection
  Gtk::FileChooserDialog dlg_out(*this, _("Select Output Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
  dlg_out.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
  dlg_out.add_button(_("_Select"), Gtk::RESPONSE_OK);
  
  if (dlg_out.run() != Gtk::RESPONSE_OK) {
    return;
  }
  
  std::string output_folder = dlg_out.get_filename();
  
  // Show info about CLI batch processing
  Gtk::MessageDialog info_dlg(*this,
                              _("Batch Processing"),
                              false, Gtk::MESSAGE_INFO);
  
  std::stringstream ss;
  ss << _("For batch processing many videos, use the command-line tool:\n\n")
     << "batch-delogo --input-folder \"" << input_folder << "\" \\\n"
     << "             --output-folder \"" << output_folder << "\" \\\n"
     << "             --config \"" << layout_manager_.get_config_file_path() << "\" \\\n"
     << "             --auto-detect\n\n"
     << _("This will process all videos in the input folder automatically.");
  
  info_dlg.set_secondary_text(ss.str());
  info_dlg.run();
}


void AutoReplaceWindow::on_close()
{
  hide();
}


bool AutoReplaceWindow::on_delete_event(GdkEventAny*)
{
  return false;  // Allow window to close
}


void AutoReplaceWindow::set_status(const std::string& message)
{
  lbl_status_->set_text(message);
}


AutoReplaceWindow::type_signal_filters_applied AutoReplaceWindow::signal_filters_applied()
{
  return signal_filters_applied_;
}

