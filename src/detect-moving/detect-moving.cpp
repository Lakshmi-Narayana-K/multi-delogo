/*
 * detect-moving: Quick check whether a video contains moving logos.
 *
 * PARALLEL PROCESSING: Divides video into 5-minute segments and processes
 * them in parallel. As soon as any segment finds movement, all threads stop
 * and the function returns true.
 *
 * Detection logic mirrors batch-delogo's Phase 2 classifier:
 *   1. COARSE SCAN — sample every N frames, record (x,y) for each template.
 *   2. PER-STEP CLASSIFICATION — for each consecutive pair of coarse positions:
 *        dist = sqrt(dx² + dy²)
 *        If dist >= motion_step_threshold (default 12 px) → both points flagged MOVING.
 *   3. DILATION — each moving point also flags its immediate neighbours.
 *   4. MOVING CONFIRMED when 2+ consecutive flagged points exist.
 *
 * Exits with code 0 regardless of result (0 = normal, 2 = error).
 *
 * Usage:
 *   detect-moving --video PATH --config PATH [OPTIONS]
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "filter-generator/VideoLayoutConfig.hpp"


// ── Template matching (single-scale, identical to batch-delogo) ─────────────
static bool detect_logo_in_frame(const cv::Mat& frame,
                                 const cv::Mat& template_img,
                                 const fg::SearchRegion& region,
                                 double threshold,
                                 int& found_x, int& found_y,
                                 double& confidence)
{
  int roi_x = std::max(0, region.x);
  int roi_y = std::max(0, region.y);
  int roi_w = std::min(region.width,  frame.cols - roi_x);
  int roi_h = std::min(region.height, frame.rows - roi_y);
  if (roi_w <= 0 || roi_h <= 0) return false;

  cv::Mat search_area = frame(cv::Rect(roi_x, roi_y, roi_w, roi_h));
  if (template_img.cols > roi_w || template_img.rows > roi_h) return false;

  cv::Mat result;
  cv::matchTemplate(search_area, template_img, result, cv::TM_CCOEFF_NORMED);

  double min_val, max_val;
  cv::Point min_loc, max_loc;
  cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);

  confidence = max_val;
  if (max_val >= threshold) {
    found_x = roi_x + max_loc.x;
    found_y = roi_y + max_loc.y;
    return true;
  }
  return false;
}


// ── Position sample ─────────────────────────────────────────────────────────
struct PosSample { int frame, x, y; };


// ── Per-detection tracking state ────────────────────────────────────────────
struct TrackState {
  std::string      name;
  cv::Mat          template_img;
  fg::SearchRegion region;
  double           threshold;

  // Rolling coarse positions (last few samples) - use deque for efficiency
  std::deque<PosSample> history;
};


// ── Classify positions using the batch-delogo Phase-2 rule ──────────────────
struct MoveResult {
  bool         confirmed = false;
  std::string  direction;
  PosSample    first_pos{};
  PosSample    last_pos{};
  double       net_dist  = 0;
};

static MoveResult classify_positions(const std::vector<PosSample>& pts,
                                     double motion_step_threshold)
{
  MoveResult r;
  if (pts.size() < 3) return r;

  std::vector<bool> moving(pts.size(), false);

  for (size_t i = 1; i < pts.size(); ++i) {
    double dx = pts[i].x - pts[i-1].x;
    double dy = pts[i].y - pts[i-1].y;
    double dist = std::sqrt(dx*dx + dy*dy);
    if (dist >= motion_step_threshold) {
      moving[i-1] = true;
      moving[i]   = true;
    }
  }

  std::vector<bool> dilated = moving;
  for (size_t i = 0; i < moving.size(); ++i) {
    if (!moving[i]) continue;
    if (i > 0)                     dilated[i-1] = true;
    if (i + 1 < moving.size())     dilated[i+1] = true;
  }

  int run = 0;
  for (size_t i = 0; i < dilated.size(); ++i) {
    if (dilated[i]) {
      ++run;
      if (run >= 2) {
        int first_moving = -1, last_moving = -1;
        for (size_t j = 0; j < moving.size(); ++j) {
          if (moving[j]) {
            if (first_moving < 0) first_moving = static_cast<int>(j);
            last_moving = static_cast<int>(j);
          }
        }
        r.confirmed = true;
        r.first_pos = pts[first_moving >= 0 ? first_moving : 0];
        r.last_pos  = pts[last_moving  >= 0 ? last_moving  : static_cast<int>(pts.size()) - 1];
        double net_dx = r.last_pos.x - r.first_pos.x;
        double net_dy = r.last_pos.y - r.first_pos.y;
        r.net_dist   = std::sqrt(net_dx*net_dx + net_dy*net_dy);
        if (std::abs(net_dx) > std::abs(net_dy))
          r.direction = (net_dx > 0) ? "left-to-right" : "right-to-left";
        else
          r.direction = (net_dy > 0) ? "top-to-bottom" : "bottom-to-top";
        return r;
      }
    } else {
      run = 0;
    }
  }
  return r;
}


// ── Fine scan with early-stop ───────────────────────────────────────────────
static MoveResult scan_range_rolling(cv::VideoCapture& cap,
                                     const TrackState& ts,
                                     int start_frame, int end_frame,
                                     int step, double motion_threshold,
                                     int& frames_checked,
                                     const std::atomic<bool>& should_stop)
{
  std::deque<PosSample> pts;
  const int FINE_HISTORY = 6;

  cap.set(cv::CAP_PROP_POS_FRAMES, start_frame);
  for (int ff = start_frame; ff <= end_frame && !should_stop; ff += step) {
    cap.set(cv::CAP_PROP_POS_FRAMES, ff);
    cv::Mat f;
    if (!cap.read(f)) break;
    ++frames_checked;

    int fx, fy; double fc;
    if (detect_logo_in_frame(f, ts.template_img, ts.region, ts.threshold, fx, fy, fc)) {
      pts.push_back({ff, fx, fy});
      if (static_cast<int>(pts.size()) > FINE_HISTORY)
        pts.pop_front();

      std::vector<PosSample> pts_vec(pts.begin(), pts.end());
      MoveResult r = classify_positions(pts_vec, motion_threshold);
      if (r.confirmed) return r;
    }
  }
  return MoveResult{};
}


// ── Thread-safe logging ─────────────────────────────────────────────────────
static void log_thread_safe(std::mutex& mtx, const std::string& msg) {
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << msg << std::flush;
}

// ── Process a single segment ────────────────────────────────────────────────
struct SegmentResult {
  bool         found = false;
  MoveResult   result;
  std::string  logo_name;
  int          segment_num;
  int          frames_checked = 0;
};

static SegmentResult process_segment(
    const std::string& video_path,
    const std::vector<TrackState>& tracks,
    int segment_num,
    int start_frame, int end_frame,
    int sample_interval, double motion_threshold, int fine_step,
    double fps,
    const std::atomic<bool>& should_stop,
    std::atomic<bool>& found_movement,
    std::mutex& log_mutex)
{
  SegmentResult seg_result;
  seg_result.segment_num = segment_num;

  double start_sec = (fps > 0) ? start_frame / fps : 0;
  double end_sec = (fps > 0) ? end_frame / fps : 0;
  int start_min = static_cast<int>(start_sec) / 60;
  int start_s = static_cast<int>(start_sec) % 60;
  int end_min = static_cast<int>(end_sec) / 60;
  int end_s = static_cast<int>(end_sec) % 60;

  std::ostringstream start_msg;
  start_msg << "  [Segment " << segment_num << "] Starting: frames " << start_frame
            << "-" << end_frame << " (" << std::setfill('0') << std::setw(2) << start_min
            << ":" << std::setw(2) << start_s << " - " << std::setw(2) << end_min
            << ":" << std::setw(2) << end_s << std::setfill(' ') << ")\n";
  log_thread_safe(log_mutex, start_msg.str());

  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) {
    std::ostringstream err_msg;
    err_msg << "  [Segment " << segment_num << "] ERROR: Cannot open video\n";
    log_thread_safe(log_mutex, err_msg.str());
    return seg_result;
  }

  const int HISTORY_KEEP = 6;
  std::vector<TrackState> local_tracks = tracks;  // Copy for thread safety
  int last_progress_frame = start_frame;

  for (int fn = start_frame; fn <= end_frame && !should_stop && !found_movement; fn += sample_interval) {
    cap.set(cv::CAP_PROP_POS_FRAMES, fn);
    cv::Mat frame;
    if (!cap.read(frame)) break;
    ++seg_result.frames_checked;

    for (auto& ts : local_tracks) {
      if (should_stop || found_movement) break;

      int x, y;
      double conf;
      bool found = detect_logo_in_frame(frame, ts.template_img, ts.region,
                                        ts.threshold, x, y, conf);

      if (found) {
        ts.history.push_back({fn, x, y});
        if (static_cast<int>(ts.history.size()) > HISTORY_KEEP)
          ts.history.pop_front();

        std::vector<PosSample> hist_vec(ts.history.begin(), ts.history.end());
        MoveResult r = classify_positions(hist_vec, motion_threshold);
        if (r.confirmed) {
          seg_result.found = true;
          seg_result.result = r;
          seg_result.logo_name = ts.name;
          found_movement = true;  // Signal all threads to stop
          
          std::ostringstream found_msg;
          found_msg << "  [Segment " << segment_num << "] MOVEMENT FOUND! Logo: " << ts.name
                    << " at frame " << fn << "\n";
          log_thread_safe(log_mutex, found_msg.str());
          
          cap.release();
          return seg_result;
        }
      } else {
        // Logo LOST — fine-scan the gap
        if (!ts.history.empty()) {
          int fine_start = std::max(start_frame, ts.history.back().frame);
          int fine_end   = std::min(end_frame, fn + sample_interval);

          MoveResult r = scan_range_rolling(cap, ts, fine_start, fine_end,
                                           fine_step, motion_threshold,
                                           seg_result.frames_checked, should_stop);
          if (r.confirmed) {
            seg_result.found = true;
            seg_result.result = r;
            seg_result.logo_name = ts.name;
            found_movement = true;
            
            std::ostringstream found_msg;
            found_msg << "  [Segment " << segment_num << "] MOVEMENT FOUND (fine scan)! Logo: " << ts.name
                      << " at frame " << fn << "\n";
            log_thread_safe(log_mutex, found_msg.str());
            
            cap.release();
            return seg_result;
          }

          cap.set(cv::CAP_PROP_POS_FRAMES, fn);
        }
        ts.history.clear();
      }
    }

    // Progress update every 50 frames checked
    if (seg_result.frames_checked % 50 == 0 && fn != last_progress_frame) {
      int progress_pct = ((fn - start_frame) * 100) / std::max(1, end_frame - start_frame);
      std::ostringstream progress_msg;
      progress_msg << "  [Segment " << segment_num << "] Progress: " << progress_pct
                   << "% (" << seg_result.frames_checked << " frames checked)\n";
      log_thread_safe(log_mutex, progress_msg.str());
      last_progress_frame = fn;
    }
  }

  std::ostringstream done_msg;
  done_msg << "  [Segment " << segment_num << "] Completed: " << seg_result.frames_checked
           << " frames checked, no movement found\n";
  log_thread_safe(log_mutex, done_msg.str());

  cap.release();
  return seg_result;
}


// ── Usage ────────────────────────────────────────────────────────────────────
static void print_usage(const char* prog)
{
  std::cout
    << "Usage: " << prog << " --video PATH --config PATH [OPTIONS]\n\n"
    << "Detects moving logos using parallel processing (5-min segments).\n"
    << "  dist >= motion-threshold on 2 consecutive coarse samples = MOVING.\n\n"
    << "Options:\n"
    << "  --video PATH              Video file to check\n"
    << "  --config PATH             Path to video_layouts.json\n"
    << "  --sample-interval N       Coarse scan: every Nth frame (default: 30)\n"
    << "  --motion-threshold PX     Per-step displacement to flag moving (default: 12)\n"
    << "  --fine-step N             Step for fine scan on logo-loss (default: 5)\n"
    << "  --segment-minutes N       Segment duration in minutes (default: 5)\n"
    << "  --verbose                 Print per-frame info\n"
    << "  --help                    Show this message\n"
    << std::endl;
}


// ── Print result ─────────────────────────────────────────────────────────────
static void print_found(const std::string& logo_name, const MoveResult& r,
                         double fps, double scan_secs, int frames_checked, int segment_num)
{
  double ts_sec = (fps > 0) ? r.last_pos.frame / fps : 0;
  int vm = static_cast<int>(ts_sec) / 60;
  int vs = static_cast<int>(ts_sec) % 60;
  std::cout << "\nMOVING LOGO DETECTED: true\n"
            << "  Logo      : " << logo_name << "\n"
            << "  Segment   : " << segment_num << "\n"
            << "  Direction : " << r.direction << "\n"
            << "  Frames    : " << r.first_pos.frame << " -> " << r.last_pos.frame
            << "  (video time " << vm << "m " << vs << "s)\n"
            << "  Start pos : (" << r.first_pos.x << ", " << r.first_pos.y << ")\n"
            << "  End pos   : (" << r.last_pos.x  << ", " << r.last_pos.y  << ")\n"
            << "  Net shift : " << std::fixed << std::setprecision(1) << r.net_dist << " px\n"
            << "  Scan time : " << std::setprecision(1) << scan_secs << "s ("
            << frames_checked << " frames checked)\n";
}


// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
  std::string video_path, config_path;
  int    sample_interval    = 30;
  double motion_threshold   = 12.0;
  int    fine_step          = 5;
  int    segment_minutes    = 2;
  bool   verbose            = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if      (arg == "--help" || arg == "-h")                { print_usage(argv[0]); return 0; }
    else if (arg == "--video"            && i+1 < argc)     video_path        = argv[++i];
    else if (arg == "--config"           && i+1 < argc)     config_path       = argv[++i];
    else if (arg == "--sample-interval"  && i+1 < argc)     sample_interval   = std::atoi(argv[++i]);
    else if (arg == "--motion-threshold" && i+1 < argc)     motion_threshold  = std::atof(argv[++i]);
    else if (arg == "--fine-step"        && i+1 < argc)     fine_step         = std::atoi(argv[++i]);
    else if (arg == "--segment-minutes"  && i+1 < argc)     segment_minutes   = std::atoi(argv[++i]);
    else if (arg == "--verbose")                             verbose           = true;
    else { std::cerr << "Unknown option: " << arg << "\n"; print_usage(argv[0]); return 2; }
  }

  if (video_path.empty() || config_path.empty()) {
    std::cerr << "Error: --video and --config are required.\n";
    print_usage(argv[0]);
    return 2;
  }

  // ── Load config ────────────────────────────────────────────────────────
  fg::VideoLayoutManager layout_mgr;
  if (!layout_mgr.load_from_file(config_path)) {
    std::cerr << "Error: Cannot load config " << config_path << "\n";
    return 2;
  }
  {
    size_t slash = config_path.rfind('/');
    if (slash != std::string::npos) {
      std::string dir = config_path.substr(0, slash);
      layout_mgr.set_reference_directory(dir + "/" + layout_mgr.get_reference_directory());
    }
  }

  // ── Open video to get properties ─────────────────────────────────────────
  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) {
    std::cerr << "Error: Cannot open video " << video_path << "\n";
    return 2;
  }

  int    frame_w     = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int    frame_h     = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  int    frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  double fps         = cap.get(cv::CAP_PROP_FPS);
  cap.release();

  std::cout << "Video : " << video_path << "\n"
            << "  Res : " << frame_w << "x" << frame_h
            << "  Frames: " << frame_count << "  FPS: " << fps << "\n";

  fg::layout_ptr layout = layout_mgr.get_layout_for_resolution(frame_w, frame_h);
  if (!layout) {
    std::cerr << "Error: No layout for " << frame_w << "x" << frame_h << "\n";
    return 2;
  }
  std::cout << "  Layout: " << layout->name << "\n";

  // ── Build tracking states ──────────────────────────────────────────────
  std::vector<TrackState> tracks;
  for (const auto& det : layout->detections) {
    cv::Mat tmpl = cv::imread(layout_mgr.get_reference_path(det.reference_image));
    if (tmpl.empty()) {
      std::cerr << "  Warning: cannot load " << det.reference_image << "\n";
      continue;
    }
    TrackState ts;
    ts.name         = det.name;
    ts.template_img = tmpl;
    ts.threshold    = det.match_threshold;

    if (det.search_quadrant >= 1 && det.search_quadrant <= 4)
      ts.region = fg::search_region_from_quadrant(frame_w, frame_h, det.search_quadrant);
    else if (det.search_region.width > 0 && det.search_region.height > 0)
      ts.region = det.search_region;
    else
      ts.region = fg::SearchRegion(0, 0, frame_w, frame_h);

    tracks.push_back(std::move(ts));
  }

  if (tracks.empty()) {
    std::cerr << "Error: no valid detection templates loaded.\n";
    return 2;
  }

  // ── Calculate segments (5 minutes each) ───────────────────────────────
  int segment_frames = static_cast<int>(segment_minutes * 60.0 * fps);
  std::vector<std::pair<int, int>> segments;
  for (int start = 0; start < frame_count; start += segment_frames) {
    int end = std::min(frame_count - 1, start + segment_frames - 1);
    segments.push_back({start, end});
  }

  std::cout << "  Templates         : " << tracks.size() << "\n"
            << "  Sample interval   : " << sample_interval << " frames\n"
            << "  Motion threshold  : " << motion_threshold << " px/step\n"
            << "  Fine step         : " << fine_step << " frames\n"
            << "  Segments          : " << segments.size() << " x " << segment_minutes << " min\n\n";

  // ── Parallel processing ─────────────────────────────────────────────────
  auto t0 = std::chrono::steady_clock::now();
  std::atomic<bool> should_stop(false);
  std::atomic<bool> found_movement(false);
  std::vector<std::thread> threads;
  std::vector<SegmentResult> results(segments.size());
  std::mutex log_mutex;

  std::cout << "  Launching " << segments.size() << " parallel threads...\n\n";

  // Launch threads for each segment
  for (size_t i = 0; i < segments.size(); ++i) {
    threads.emplace_back([&, i]() {
      results[i] = process_segment(
          video_path, tracks, static_cast<int>(i) + 1,
          segments[i].first, segments[i].second,
          sample_interval, motion_threshold, fine_step, fps,
          should_stop, found_movement, log_mutex);
    });
  }

  std::cout << "  All threads started. Waiting for results...\n\n";

  // Wait for any thread to find movement or all to complete
  for (auto& t : threads) {
    t.join();
  }

  std::cout << "\n  All threads completed.\n";

  // ── Check results ───────────────────────────────────────────────────────
  auto elapsed = std::chrono::steady_clock::now() - t0;
  double secs  = std::chrono::duration<double>(elapsed).count();

  for (const auto& res : results) {
    if (res.found) {
      int total_frames = 0;
      for (const auto& r : results) total_frames += r.frames_checked;
      print_found(res.logo_name, res.result, fps, secs, total_frames, res.segment_num);
      return 0;
    }
  }

  // ── No movement found ──────────────────────────────────────────────────
  int total_frames = 0;
  for (const auto& res : results) total_frames += res.frames_checked;
  double total_sec = (fps > 0) ? frame_count / fps : 0;
  int vm = static_cast<int>(total_sec) / 60;
  int vs = static_cast<int>(total_sec) % 60;
  std::cout << "\nMOVING LOGO DETECTED: false\n"
            << "  Video duration : " << vm << "m " << vs << "s\n"
            << "  Scan time      : " << std::fixed << std::setprecision(1)
            << secs << "s (" << total_frames << " frames checked across " << segments.size() << " segments)\n"
            << "  No movement above " << motion_threshold << "px/step detected.\n";
  return 0;
}
