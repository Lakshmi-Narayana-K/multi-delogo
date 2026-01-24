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
#ifndef MDL_FILTER_PANELS_H
#define MDL_FILTER_PANELS_H

#include <memory>

#include <gtkmm.h>

#include "filter-generator/Filters.hpp"
#include "filter-generator/ImagePreset.hpp"

#include "common/Rectangle.hpp"

#include "FilterPanelFactory.hpp"


namespace mdl {
  class FilterPanelNoParameters : public FilterPanel
  {
  protected:
    FilterPanelNoParameters(int start_frame, int max_frame);

  public:
    Parameters get_parameters() const override;
    void set_parameters(const Parameters& parameters) override;
  };


  class FilterPanelNull : public FilterPanelNoParameters
  {
  public:
    FilterPanelNull(int start_frame, int max_frame);

    bool creates_filter() const override;
    fg::filter_ptr get_filter() const override;
  };


  class FilterPanelCut : public FilterPanelNoParameters
  {
  public:
    FilterPanelCut(int start_frame, int max_frame);

    fg::filter_ptr get_filter() const override;
  };


  class FilterPanelReview : public FilterPanelNoParameters
  {
  public:
    FilterPanelReview(int start_frame, int max_frame);

    fg::filter_ptr get_filter() const override;
  };


  class FilterPanelWithParameters: public FilterPanel
  {
  protected:
    FilterPanelWithParameters(int start_frame, int max_frame);

    void add_widget(Gtk::Widget& widget,
                    const Glib::ustring& label, int row);
  };


  class FilterPanelSpeed : public FilterPanelWithParameters
  {
  public:
    FilterPanelSpeed(int start_frame, int max_frame);
    FilterPanelSpeed(int start_frame, int max_frame,
                     std::shared_ptr<fg::SpeedFilter> filter);

    fg::filter_ptr get_filter() const override;
    Parameters get_parameters() const override;
    void set_parameters(const Parameters& parameters) override;

  private:
    FilterPanelSpeed(int start_frame, int max_frame,
                     double factor);

    Gtk::SpinButton txt_factor_;
  };


  class FilterPanelRectangular : public FilterPanelWithParameters
  {
  protected:
    FilterPanelRectangular(int start_frame, int max_frame,
                           int frame_width, int frame_height);
    FilterPanelRectangular(int start_frame, int max_frame,
                           std::shared_ptr<fg::RectangularFilter> filter,
                           int frame_width, int frame_height);
    FilterPanelRectangular(int start_frame, int max_frame,
                           int x, int y, int width, int height,
                           int frame_width, int frame_height);

  public:
    Parameters get_parameters() const override;
    void set_parameters(const Parameters& parameters) override;

  protected:
    Gtk::SpinButton txt_x_;
    Gtk::SpinButton txt_y_;
    Gtk::SpinButton txt_width_;
    Gtk::SpinButton txt_height_;

  private:
    Glib::RefPtr<Gtk::Adjustment> create_adjustment(int start_value, int max);
  };


  class FilterPanelDelogo : public FilterPanelRectangular
  {
  public:
    FilterPanelDelogo(int start_frame, int max_frame,
                      int frame_width, int frame_height);
    FilterPanelDelogo(int start_frame, int max_frame,
                      std::shared_ptr<fg::DelogoFilter> filter,
                      int frame_width, int frame_height);

    fg::filter_ptr get_filter() const override;
  };


  class FilterPanelDrawbox : public FilterPanelRectangular
  {
  public:
    FilterPanelDrawbox(int start_frame, int max_frame,
                       int frame_width, int frame_height);
    FilterPanelDrawbox(int start_frame, int max_frame,
                       std::shared_ptr<fg::DrawboxFilter> filter,
                       int frame_width, int frame_height);

    fg::filter_ptr get_filter() const override;
  };


  class FilterPanelImageOverlay : public FilterPanelRectangular
  {
  public:
    FilterPanelImageOverlay(int start_frame, int max_frame,
                            int frame_width, int frame_height);
    FilterPanelImageOverlay(int start_frame, int max_frame,
                            std::shared_ptr<fg::ImageOverlayFilter> filter,
                            int frame_width, int frame_height);

    fg::filter_ptr get_filter() const override;

    std::string get_image_path() const;
    void set_image_path(const std::string& path);
    
    // Set the preset manager (shared across all panels)
    static void set_preset_manager(std::shared_ptr<fg::ImagePresetManager> manager);
    static std::shared_ptr<fg::ImagePresetManager> get_preset_manager();

  private:
    FilterPanelImageOverlay(int start_frame, int max_frame,
                            int x, int y, int width, int height,
                            const std::string& image_path,
                            int frame_width, int frame_height);

    // Preset selection
    Gtk::ComboBoxText cmb_preset_;
    Gtk::Label lbl_or_;
    
    // Manual selection (fallback)
    Gtk::Entry txt_image_path_;
    Gtk::Button btn_browse_;
    
    std::string image_path_;
    int frame_width_;
    int frame_height_;
    
    static std::shared_ptr<fg::ImagePresetManager> preset_manager_;

    void on_preset_changed();
    void on_browse_clicked();
    void populate_preset_dropdown();
    void apply_preset(const std::string& preset_id);
  };
}

#endif // MDL_FILTER_PANELS_H
