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
#include <istream>
#include <ostream>
#include <limits>
#include <algorithm>

#include <boost/optional.hpp>
#include <boost/algorithm/string.hpp>

#include "Exceptions.hpp"
#include "IOUtils.hpp"
#include "Filters.hpp"
#include "FilterFactory.hpp"
#include "FilterList.hpp"

using namespace fg;


void FilterList::insert(int start_frame, filter_ptr filter)
{
  insert(start_frame, NO_END_FRAME, filter);
}


void FilterList::insert(int start_frame, int end_frame, filter_ptr filter)
{
  // Remove existing filter at same start_frame (if any)
  remove(start_frame);
  filters_.emplace_back(start_frame, end_frame, filter);
  sort_filters();
}


void FilterList::remove(int start_frame)
{
  auto it = std::find_if(filters_.begin(), filters_.end(),
    [start_frame](const FilterEntry& entry) {
      return entry.start_frame == start_frame;
    });
  if (it != filters_.end()) {
    filters_.erase(it);
  }
}


void FilterList::remove_by_index(size_type index)
{
  if (index < filters_.size()) {
    filters_.erase(filters_.begin() + index);
  }
}


void FilterList::change_start_frame(int old_start_frame, int new_start_frame)
{
  auto it = std::find_if(filters_.begin(), filters_.end(),
    [old_start_frame](const FilterEntry& entry) {
      return entry.start_frame == old_start_frame;
    });
  if (it == filters_.end()) {
    return;
  }

  // Remove any existing filter at the new start frame
  remove(new_start_frame);

  it->start_frame = new_start_frame;
  sort_filters();
}


void FilterList::change_end_frame(int start_frame, int new_end_frame)
{
  auto it = std::find_if(filters_.begin(), filters_.end(),
    [start_frame](const FilterEntry& entry) {
      return entry.start_frame == start_frame;
    });
  if (it != filters_.end()) {
    it->end_frame = new_end_frame;
  }
}


void FilterList::update_entry(size_type index, int start_frame, int end_frame, filter_ptr filter)
{
  if (index < filters_.size()) {
    filters_[index].start_frame = start_frame;
    filters_[index].end_frame = end_frame;
    filters_[index].filter = filter;
    sort_filters();
  }
}


bool FilterList::empty() const
{
  return filters_.empty();
}


FilterList::size_type FilterList::size() const
{
  return filters_.size();
}


void FilterList::clear()
{
  filters_.clear();
}


FilterList::const_iterator FilterList::begin() const
{
  return filters_.begin();
}


FilterList::const_iterator FilterList::end() const
{
  return filters_.end();
}


FilterList::maybe_type FilterList::get_by_start_frame(int start_frame) const
{
  auto it = std::find_if(filters_.begin(), filters_.end(),
    [start_frame](const FilterEntry& entry) {
      return entry.start_frame == start_frame;
    });
  if (it == filters_.end()) {
    return boost::none;
  }

  return boost::make_optional(std::make_pair(it->start_frame, it->filter));
}


FilterList::maybe_type FilterList::get_by_position(size_type position) const
{
  if (position >= filters_.size()) {
    return boost::none;
  }

  const FilterEntry& entry = filters_[position];
  return boost::make_optional(std::make_pair(entry.start_frame, entry.filter));
}


int FilterList::get_position(int start_frame) const
{
  for (size_type i = 0; i < filters_.size(); ++i) {
    if (filters_[i].start_frame == start_frame) {
      return static_cast<int>(i);
    }
  }
  return -1;
}


FilterList::maybe_type FilterList::get_filter_for_frame(int frame) const
{
  // For backward compatibility: returns first filter that covers this frame
  for (const auto& entry : filters_) {
    int effective_end = (entry.end_frame == NO_END_FRAME)
      ? std::numeric_limits<int>::max()
      : entry.end_frame;

    if (frame >= entry.start_frame && frame <= effective_end) {
      return boost::make_optional(std::make_pair(entry.start_frame, entry.filter));
    }
  }
  return boost::none;
}


FilterList::maybe_entry_type FilterList::get_entry_by_position(size_type position) const
{
  if (position >= filters_.size()) {
    return boost::none;
  }
  return boost::make_optional(filters_[position]);
}


FilterList::maybe_entry_type FilterList::get_entry_by_start_frame(int start_frame) const
{
  auto it = std::find_if(filters_.begin(), filters_.end(),
    [start_frame](const FilterEntry& entry) {
      return entry.start_frame == start_frame;
    });
  if (it == filters_.end()) {
    return boost::none;
  }
  return boost::make_optional(*it);
}


std::vector<FilterEntry> FilterList::get_filters_for_frame(int frame) const
{
  std::vector<FilterEntry> result;
  for (const auto& entry : filters_) {
    int effective_end = (entry.end_frame == NO_END_FRAME)
      ? std::numeric_limits<int>::max()
      : entry.end_frame;

    if (frame >= entry.start_frame && frame <= effective_end) {
      result.push_back(entry);
    }
  }
  return result;
}


bool FilterList::has_review_filter() const
{
  return std::any_of(filters_.begin(), filters_.end(), [](const FilterEntry& entry) {
      return entry.filter->type() == FilterType::REVIEW;
    });
}


void FilterList::load(std::istream& in)
{
  std::string line;
  while (fg::getline(in, line)) {
    load_line(line);
  }
}


void FilterList::load_line(const std::string& line)
{
  // New format: start_frame;end_frame;filter_type;params...
  // Legacy format: start_frame;filter_type;params...

  std::vector<std::string> parts;
  boost::split(parts, line, boost::is_any_of(";"), boost::token_compress_off);

  if (parts.size() < 2) {
    throw InvalidFilterException();
  }

  try {
    int start_frame = std::stoi(parts[0]);
    int end_frame = NO_END_FRAME;
    std::string filter_str;

    // Check if second part is a number (new format with end_frame)
    // or a filter type (legacy format)
    bool is_new_format = false;
    try {
      end_frame = std::stoi(parts[1]);
      // If we get here, parts[1] is a number, so this is new format
      is_new_format = true;
    } catch (std::invalid_argument&) {
      // parts[1] is not a number, so this is legacy format
      end_frame = NO_END_FRAME;
      is_new_format = false;
    }

    if (is_new_format) {
      // New format: start;end;type;params...
      // Reconstruct filter string from parts[2] onwards
      if (parts.size() < 3) {
        throw InvalidFilterException();
      }
      filter_str = parts[2];
      for (size_t i = 3; i < parts.size(); ++i) {
        filter_str += ";" + parts[i];
      }
    } else {
      // Legacy format: start;type;params...
      // Reconstruct filter string from parts[1] onwards
      filter_str = parts[1];
      for (size_t i = 2; i < parts.size(); ++i) {
        filter_str += ";" + parts[i];
      }
    }

    filter_ptr filter = FilterFactory::load(filter_str);
    filters_.emplace_back(start_frame, end_frame, filter);
  } catch (std::invalid_argument& e) {
    throw InvalidFilterException();
  }

  sort_filters();
}


void FilterList::save(std::ostream& out) const
{
  for (const auto& entry : filters_) {
    // New format: start_frame;end_frame;filter_type;params...
    out << entry.start_frame << ';' << entry.end_frame << ';' << entry.filter->save_str() << '\n';
  }
}


void FilterList::sort_filters()
{
  std::stable_sort(filters_.begin(), filters_.end(),
    [](const FilterEntry& a, const FilterEntry& b) {
      return a.start_frame < b.start_frame;
    });
}
