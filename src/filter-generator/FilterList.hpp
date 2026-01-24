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
#ifndef FG_FILTER_LIST_H
#define FG_FILTER_LIST_H

#include <memory>
#include <string>
#include <vector>
#include <istream>
#include <ostream>

#include <boost/optional.hpp>

#include "Filters.hpp"


namespace fg {
  // Entry representing a filter with its time range
  struct FilterEntry {
    int start_frame;
    int end_frame;  // NO_END_FRAME (-1) means extends to end of video
    filter_ptr filter;

    FilterEntry(int start, int end, filter_ptr f)
      : start_frame(start), end_frame(end), filter(f) {}

    // For backward compatibility: creates entry with no end frame
    FilterEntry(int start, filter_ptr f)
      : start_frame(start), end_frame(NO_END_FRAME), filter(f) {}
  };

  class FilterList
  {
  public:
    // Legacy types for backward compatibility
    typedef std::pair<int, filter_ptr> value_type;
    typedef boost::optional<value_type> maybe_type;
    typedef std::vector<FilterEntry>::size_type size_type;
    typedef std::vector<FilterEntry>::const_iterator const_iterator;

    // New type for full filter entry access
    typedef boost::optional<FilterEntry> maybe_entry_type;

    FilterList() = default;

    // No copying
    FilterList (const FilterList&) = delete;
    FilterList& operator=(const FilterList&) = delete;

    // Legacy insert (for backward compatibility) - uses NO_END_FRAME
    void insert(int start_frame, filter_ptr filter);

    // New insert with explicit end frame
    void insert(int start_frame, int end_frame, filter_ptr filter);

    void remove(int start_frame);
    void remove_by_index(size_type index);
    void change_start_frame(int old_start_frame, int new_start_frame);
    void change_end_frame(int start_frame, int new_end_frame);

    // Update filter entry by index
    void update_entry(size_type index, int start_frame, int end_frame, filter_ptr filter);

    bool empty() const;
    size_type size() const;
    void clear();

    const_iterator begin() const;
    const_iterator end() const;

    // Legacy accessors (return value_type for compatibility)
    maybe_type get_by_start_frame(int start_frame) const;
    maybe_type get_by_position(size_type position) const;
    int get_position(int start_frame) const;
    maybe_type get_filter_for_frame(int frame) const;

    // New accessors for full entry with end_frame
    maybe_entry_type get_entry_by_position(size_type position) const;
    maybe_entry_type get_entry_by_start_frame(int start_frame) const;

    // Get all filters active at a specific frame (for concurrent filters)
    std::vector<FilterEntry> get_filters_for_frame(int frame) const;

    bool has_review_filter() const;

    void load(std::istream& in);
    void save(std::ostream& out) const;


  private:
    std::vector<FilterEntry> filters_;

    void load_line(const std::string& line);
    void sort_filters();
  };
}

#endif // FG_FILTER_LIST_H
