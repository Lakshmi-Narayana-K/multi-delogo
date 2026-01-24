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
#include <gtkmm.h>
#include <glibmm/i18n.h>

#include "filter-generator/Filters.hpp"

#include "FilterPanels.hpp"

using namespace mdl;


FilterPanel::FilterPanel(int start_frame, int max_frame)
  : max_frame_(max_frame)
{
  set_orientation(Gtk::ORIENTATION_VERTICAL);
  set_row_spacing(6);
  set_column_spacing(4);

  // Start frame
  lbl_start_frame_.set_label(_("_Start frame:"));
  lbl_start_frame_.set_use_underline();
  lbl_start_frame_.set_mnemonic_widget(txt_start_frame_);

  txt_start_frame_.configure(Gtk::Adjustment::create(start_frame, 1, max_frame), 10, 0);
  txt_start_frame_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanel::on_start_frame_changed));

  lbl_start_frame_.set_halign(Gtk::ALIGN_END);
  attach(lbl_start_frame_, 0, -1, 1, 1);
  attach_next_to(txt_start_frame_, lbl_start_frame_, Gtk::POS_RIGHT, 1, 1);

  // End frame
  lbl_end_frame_.set_label(_("_End frame:"));
  lbl_end_frame_.set_use_underline();
  lbl_end_frame_.set_mnemonic_widget(txt_end_frame_);

  txt_end_frame_.configure(Gtk::Adjustment::create(max_frame, 1, max_frame), 10, 0);
  txt_end_frame_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanel::on_end_frame_changed));

  chk_no_end_frame_.set_label(_("To end of video"));
  chk_no_end_frame_.set_active(true);  // Default to no end frame
  chk_no_end_frame_.signal_toggled().connect(
    sigc::mem_fun(*this, &FilterPanel::on_no_end_frame_toggled));

  lbl_end_frame_.set_halign(Gtk::ALIGN_END);
  attach(lbl_end_frame_, 0, -2, 1, 1);
  attach_next_to(txt_end_frame_, lbl_end_frame_, Gtk::POS_RIGHT, 1, 1);
  attach_next_to(chk_no_end_frame_, txt_end_frame_, Gtk::POS_RIGHT, 1, 1);

  // Initially disable end frame input since "to end of video" is checked
  txt_end_frame_.set_sensitive(false);
}


FilterPanel::~FilterPanel()
{
}


bool FilterPanel::creates_filter() const
{
  return true;
}


void FilterPanel::set_start_frame(int start_frame)
{
  txt_start_frame_.set_value(start_frame);
}


void FilterPanel::set_end_frame(int end_frame)
{
  if (end_frame == fg::NO_END_FRAME) {
    chk_no_end_frame_.set_active(true);
    txt_end_frame_.set_sensitive(false);
  } else {
    chk_no_end_frame_.set_active(false);
    txt_end_frame_.set_sensitive(true);
    txt_end_frame_.set_value(end_frame);
  }
}


int FilterPanel::get_end_frame() const
{
  if (chk_no_end_frame_.get_active()) {
    return fg::NO_END_FRAME;
  }
  return txt_end_frame_.get_value_as_int();
}


FilterPanel::type_signal_start_frame_changed FilterPanel::signal_start_frame_changed()
{
  return signal_start_frame_changed_;
}


FilterPanel::type_signal_end_frame_changed FilterPanel::signal_end_frame_changed()
{
  return signal_end_frame_changed_;
}


FilterPanel::type_signal_parameters_changed FilterPanel::signal_parameters_changed()
{
  return signal_parameters_changed_;
}


void FilterPanel::on_start_frame_changed()
{
  signal_start_frame_changed_.emit(txt_start_frame_.get_value_as_int());
}


void FilterPanel::on_end_frame_changed()
{
  signal_end_frame_changed_.emit(get_end_frame());
}


void FilterPanel::on_no_end_frame_toggled()
{
  bool no_end = chk_no_end_frame_.get_active();
  txt_end_frame_.set_sensitive(!no_end);
  signal_end_frame_changed_.emit(get_end_frame());
}


void FilterPanel::on_parameters_changed()
{
  signal_parameters_changed_.emit(get_parameters());
}


FilterPanelNoParameters::FilterPanelNoParameters(int start_frame, int max_frame)
  : FilterPanel(start_frame, max_frame)
{
}


FilterPanel::Parameters FilterPanelNoParameters::get_parameters() const
{
  return FilterPanel::Parameters(FilterPanel::NoParameters());
}


void FilterPanelNoParameters::set_parameters(const Parameters& parameters)
{
  // nothing to do
}


FilterPanelNull::FilterPanelNull(int start_frame, int max_frame)
  : FilterPanelNoParameters(start_frame, max_frame)
{
}


bool FilterPanelNull::creates_filter() const
{
  return false;
}


fg::filter_ptr FilterPanelNull::get_filter() const
{
  return fg::filter_ptr(new fg::NullFilter());
}


FilterPanelCut::FilterPanelCut(int start_frame, int max_frame)
  : FilterPanelNoParameters(start_frame, max_frame)
{
}


fg::filter_ptr FilterPanelCut::get_filter() const
{
  return fg::filter_ptr(new fg::CutFilter());
}


FilterPanelReview::FilterPanelReview(int start_frame, int max_frame)
  : FilterPanelNoParameters(start_frame, max_frame)
{
}


fg::filter_ptr FilterPanelReview::get_filter() const
{
  return fg::filter_ptr(new fg::ReviewFilter());
}


FilterPanelWithParameters::FilterPanelWithParameters(int start_frame, int max_frame)
  : FilterPanel(start_frame, max_frame)
{
}


void FilterPanelWithParameters::add_widget(Gtk::Widget& widget,
                                           const Glib::ustring& label, int row)
{
  Gtk::Label* l = Gtk::manage(new Gtk::Label(label, true));
  l->set_mnemonic_widget(widget);
  l->set_halign(Gtk::ALIGN_END);
  attach(*l, 0, row, 1, 1);
  attach_next_to(widget, *l, Gtk::POS_RIGHT, 1, 1);
}


FilterPanelSpeed::FilterPanelSpeed(int start_frame, int max_frame)
  : FilterPanelSpeed(start_frame, max_frame, 1.0)
{
}


FilterPanelSpeed::FilterPanelSpeed(int start_frame, int max_frame,
                                   std::shared_ptr<fg::SpeedFilter> filter)
  : FilterPanelSpeed(start_frame, max_frame, filter->factor())
{
}


FilterPanelSpeed::FilterPanelSpeed(int start_frame, int max_frame,
                                   double factor)
  : FilterPanelWithParameters(start_frame, max_frame)
{
  txt_factor_.configure(Gtk::Adjustment::create(factor, 0.5, 10.0, 0.1), 0.1, 1);

  add_widget(txt_factor_, _("_speed:"), 0);

  txt_factor_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanelSpeed::on_parameters_changed));
}


fg::filter_ptr FilterPanelSpeed::get_filter() const
{
  return fg::filter_ptr(new fg::SpeedFilter(txt_factor_.get_value()));
}


FilterPanel::Parameters FilterPanelSpeed::get_parameters() const
{
  return Parameters(txt_factor_.get_value());
}


void FilterPanelSpeed::set_parameters(const Parameters& parameters)
{
  if (boost::variant2::holds_alternative<double>(parameters)) {
    double factor = boost::variant2::get<double>(parameters);
    txt_factor_.set_value(factor);
  }
}


FilterPanelRectangular::FilterPanelRectangular(int start_frame, int max_frame,
                                               int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame,
                           0, 0, 0, 0,
                           frame_width, frame_height)
{
}


FilterPanelRectangular::FilterPanelRectangular(int start_frame, int max_frame,
                                               std::shared_ptr<fg::RectangularFilter> filter,
                                               int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame,
                           filter->x(), filter->y(), filter->width(), filter->height(),
                           frame_width, frame_height)
{
}


FilterPanelRectangular::FilterPanelRectangular(int start_frame, int max_frame,
                                               int x, int y, int width, int height,
                                               int frame_width, int frame_height)
  : FilterPanelWithParameters(start_frame, max_frame)
{
  txt_x_.configure(create_adjustment(x, frame_width - 1), 10, 0);
  txt_y_.configure(create_adjustment(y, frame_height - 1), 10, 0);
  txt_width_.configure(create_adjustment(width, frame_width), 10, 0);
  txt_height_.configure(create_adjustment(height, frame_height), 10, 0);

  add_widget(txt_x_, _("_x:"), 0);
  add_widget(txt_y_, _("_y:"), 1);
  add_widget(txt_width_, _("_width:"), 2);
  add_widget(txt_height_, _("_height:"), 3);

  txt_x_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanelRectangular::on_parameters_changed));
  txt_y_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanelRectangular::on_parameters_changed));
  txt_width_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanelRectangular::on_parameters_changed));
  txt_height_.signal_value_changed().connect(
    sigc::mem_fun(*this, &FilterPanelRectangular::on_parameters_changed));
}


Glib::RefPtr<Gtk::Adjustment> FilterPanelRectangular::create_adjustment(int start_value, int max)
{
  return Gtk::Adjustment::create(start_value, 0, max);
}


FilterPanel::Parameters FilterPanelRectangular::get_parameters() const
{
  Rectangle rect = {.x = txt_x_.get_value(),
                    .y = txt_y_.get_value(),
                    .width = txt_width_.get_value(),
                    .height = txt_height_.get_value()};
  return FilterPanel::Parameters(rect);
}


void FilterPanelRectangular::set_parameters(const Parameters& parameters)
{
  if (boost::variant2::holds_alternative<Rectangle>(parameters)) {
    Rectangle rect = boost::variant2::get<Rectangle>(parameters);
    txt_x_.set_value(rect.x);
    txt_y_.set_value(rect.y);
    txt_width_.set_value(rect.width);
    txt_height_.set_value(rect.height);
  }
}


FilterPanelDelogo::FilterPanelDelogo(int start_frame, int max_frame,
                                     int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame, frame_width, frame_height)
{
}


FilterPanelDelogo::FilterPanelDelogo(int start_frame, int max_frame,
                                     std::shared_ptr<fg::DelogoFilter> filter,
                                     int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame, filter, frame_width, frame_height)
{
}


fg::filter_ptr FilterPanelDelogo::get_filter() const
{
  return fg::filter_ptr(new fg::DelogoFilter(txt_x_.get_value_as_int(),
                                             txt_y_.get_value_as_int(),
                                             txt_width_.get_value_as_int(),
                                             txt_height_.get_value_as_int()));
}


FilterPanelDrawbox::FilterPanelDrawbox(int start_frame, int max_frame,
                                       int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame, frame_width, frame_height)
{
}


FilterPanelDrawbox::FilterPanelDrawbox(int start_frame, int max_frame,
                                       std::shared_ptr<fg::DrawboxFilter> filter,
                                       int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame, filter, frame_width, frame_height)
{
}


fg::filter_ptr FilterPanelDrawbox::get_filter() const
{
  return fg::filter_ptr(new fg::DrawboxFilter(txt_x_.get_value_as_int(),
                                              txt_y_.get_value_as_int(),
                                              txt_width_.get_value_as_int(),
                                              txt_height_.get_value_as_int()));
}


// Static member initialization
std::shared_ptr<fg::ImagePresetManager> FilterPanelImageOverlay::preset_manager_ = nullptr;


void FilterPanelImageOverlay::set_preset_manager(std::shared_ptr<fg::ImagePresetManager> manager)
{
  preset_manager_ = manager;
}


std::shared_ptr<fg::ImagePresetManager> FilterPanelImageOverlay::get_preset_manager()
{
  return preset_manager_;
}


FilterPanelImageOverlay::FilterPanelImageOverlay(int start_frame, int max_frame,
                                                 int frame_width, int frame_height)
  : FilterPanelImageOverlay(start_frame, max_frame,
                            0, 0, 0, 0, "",
                            frame_width, frame_height)
{
}


FilterPanelImageOverlay::FilterPanelImageOverlay(int start_frame, int max_frame,
                                                 std::shared_ptr<fg::ImageOverlayFilter> filter,
                                                 int frame_width, int frame_height)
  : FilterPanelImageOverlay(start_frame, max_frame,
                            filter->x(), filter->y(), filter->width(), filter->height(),
                            filter->image_path(),
                            frame_width, frame_height)
{
}


FilterPanelImageOverlay::FilterPanelImageOverlay(int start_frame, int max_frame,
                                                 int x, int y, int width, int height,
                                                 const std::string& image_path,
                                                 int frame_width, int frame_height)
  : FilterPanelRectangular(start_frame, max_frame, x, y, width, height, frame_width, frame_height)
  , image_path_(image_path)
  , frame_width_(frame_width)
  , frame_height_(frame_height)
{
  // Row 4: Preset dropdown
  Gtk::Box* preset_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
  
  cmb_preset_.set_hexpand(true);
  populate_preset_dropdown();
  cmb_preset_.signal_changed().connect(
    sigc::mem_fun(*this, &FilterPanelImageOverlay::on_preset_changed));
  
  preset_box->pack_start(cmb_preset_, true, true);
  add_widget(*preset_box, _("_Preset:"), 4);
  
  // Row 5: "Or browse manually" label and controls
  Gtk::Box* image_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));

  txt_image_path_.set_text(image_path);
  txt_image_path_.set_hexpand(true);
  txt_image_path_.set_placeholder_text(_("Or browse for image..."));

  btn_browse_.set_label(_("Browse..."));
  btn_browse_.signal_clicked().connect(
    sigc::mem_fun(*this, &FilterPanelImageOverlay::on_browse_clicked));

  image_box->pack_start(txt_image_path_, true, true);
  image_box->pack_start(btn_browse_, false, false);

  add_widget(*image_box, _("_Image:"), 5);
}


void FilterPanelImageOverlay::populate_preset_dropdown()
{
  cmb_preset_.remove_all();
  cmb_preset_.append("", _("-- Select Preset --"));
  
  if (preset_manager_) {
    for (const auto& preset : preset_manager_->get_all_presets()) {
      cmb_preset_.append(preset.id, preset.name + " (" + preset.id + ")");
    }
  }
  
  cmb_preset_.set_active(0);
}


void FilterPanelImageOverlay::on_preset_changed()
{
  std::string preset_id = cmb_preset_.get_active_id();
  if (!preset_id.empty()) {
    apply_preset(preset_id);
  }
}


void FilterPanelImageOverlay::apply_preset(const std::string& preset_id)
{
  if (!preset_manager_) return;
  
  auto preset = preset_manager_->get_preset(preset_id);
  if (!preset) return;
  
  // Calculate absolute position from margins if needed
  preset->calculate_absolute_position(frame_width_, frame_height_);
  
  // Apply position and dimensions
  txt_x_.set_value(preset->x);
  txt_y_.set_value(preset->y);
  txt_width_.set_value(preset->width);
  txt_height_.set_value(preset->height);
  
  // Apply image path
  txt_image_path_.set_text(preset->image_path);
  image_path_ = preset->image_path;
  
  on_parameters_changed();
}


fg::filter_ptr FilterPanelImageOverlay::get_filter() const
{
  return fg::filter_ptr(new fg::ImageOverlayFilter(txt_x_.get_value_as_int(),
                                                   txt_y_.get_value_as_int(),
                                                   txt_width_.get_value_as_int(),
                                                   txt_height_.get_value_as_int(),
                                                   txt_image_path_.get_text()));
}


std::string FilterPanelImageOverlay::get_image_path() const
{
  return txt_image_path_.get_text();
}


void FilterPanelImageOverlay::set_image_path(const std::string& path)
{
  txt_image_path_.set_text(path);
  image_path_ = path;
}


void FilterPanelImageOverlay::on_browse_clicked()
{
  Gtk::FileChooserDialog dialog(_("Select Image"),
                                 Gtk::FILE_CHOOSER_ACTION_OPEN);
  dialog.set_transient_for(*dynamic_cast<Gtk::Window*>(get_toplevel()));

  dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
  dialog.add_button(_("_Open"), Gtk::RESPONSE_OK);

  // Add image file filters
  auto filter_images = Gtk::FileFilter::create();
  filter_images->set_name(_("Image files"));
  filter_images->add_mime_type("image/png");
  filter_images->add_mime_type("image/jpeg");
  filter_images->add_mime_type("image/gif");
  filter_images->add_mime_type("image/bmp");
  filter_images->add_pattern("*.png");
  filter_images->add_pattern("*.jpg");
  filter_images->add_pattern("*.jpeg");
  filter_images->add_pattern("*.gif");
  filter_images->add_pattern("*.bmp");
  dialog.add_filter(filter_images);

  auto filter_all = Gtk::FileFilter::create();
  filter_all->set_name(_("All files"));
  filter_all->add_pattern("*");
  dialog.add_filter(filter_all);

  if (dialog.run() == Gtk::RESPONSE_OK) {
    txt_image_path_.set_text(dialog.get_filename());
    // Clear preset selection since user manually selected
    cmb_preset_.set_active(0);
    on_parameters_changed();
  }
}
