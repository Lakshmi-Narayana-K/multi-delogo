/*
 * detect-moving: Quick check whether a video contains moving logos.
 *
 * Detection logic mirrors batch-delogo's Phase 2 classifier:
 *
 *   1. COARSE SCAN — sample every N frames, record (x,y) for each template.
 *   2. PER-STEP CLASSIFICATION — for each consecutive pair of coarse positions:
 *        dist = sqrt(dx² + dy²)
 *        If dist >= motion_step_threshold (default 12 px) → both points flagged MOVING.
 *   3. DILATION — each moving point also flags its immediate neighbours.
 *   4. MOVING CONFIRMED when 2+ consecutive flagged points exist (i.e. two steps
 *      in a row both >= threshold).  A single isolated jump is treated as noise.
 *
 *   Additionally, when a tracked logo is LOST, a fine scan (every fine_step frames)
 *   is run around the gap.  The same 12 px / 2-consecutive rule is applied to the
 *   fine positions to catch fast transitions (< 1 s) invisible to the coarse scan.
 *
 * Exits with code 0 regardless of result (0 = normal, 2 = error).
 *
 * Usage:
 *   detect-moving --video PATH --config PATH [OPTIONS]
 */

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
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

  // Rolling coarse positions (last few samples)
  std::vector<PosSample> history;

  // Streak of consecutive steps that each exceeded the motion threshold
  int consec_moves = 0;
};


// ── Classify positions using the batch-delogo Phase-2 rule ──────────────────
// Returns true (MOVING confirmed) as soon as 2 consecutive steps each have
// dist >= motion_step_threshold (mirrors the "dilate + 2-consecutive" test).
// Also fills `result` with the first confirmed moving sample.
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
  if (pts.size() < 3) return r;  // need at least 3 pts (2 steps)

  std::vector<bool> moving(pts.size(), false);

  // Mark each point pair whose step exceeds the threshold
  for (size_t i = 1; i < pts.size(); ++i) {
    double dx = pts[i].x - pts[i-1].x;
    double dy = pts[i].y - pts[i-1].y;
    double dist = std::sqrt(dx*dx + dy*dy);
    if (dist >= motion_step_threshold) {
      moving[i-1] = true;
      moving[i]   = true;
    }
  }

  // Dilate by one (same as batch-delogo)
  std::vector<bool> dilated = moving;
  for (size_t i = 0; i < moving.size(); ++i) {
    if (!moving[i]) continue;
    if (i > 0)                     dilated[i-1] = true;
    if (i + 1 < moving.size())     dilated[i+1] = true;
  }

  // Find first run of 2+ consecutive dilated points → MOVING
  int run = 0;
  for (size_t i = 0; i < dilated.size(); ++i) {
    if (dilated[i]) {
      ++run;
      if (run >= 2) {
        // Movement confirmed.
        // Report positions from the ORIGINAL moving[] range (not the dilated
        // neighbours) so that first_pos/last_pos reflect the actual slide.
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


// ── Fine scan with early-stop: collects positions and confirms movement ASAP ─
// Applies the same 12 px / 2-consecutive rule on a rolling basis.
// Stops and returns confirmed=true as soon as movement is confirmed.
// If the full range is scanned without confirmation, returns confirmed=false
// with all collected positions for the caller to inspect if needed.
static MoveResult scan_range_rolling(cv::VideoCapture& cap,
                                     const TrackState& ts,
                                     int start_frame, int end_frame,
                                     int step, double motion_threshold,
                                     int& frames_checked, bool verbose,
                                     const char* label)
{
  std::vector<PosSample> pts;
  const int FINE_HISTORY = 6;

  for (int ff = start_frame; ff <= end_frame; ff += step) {
    cap.set(cv::CAP_PROP_POS_FRAMES, ff);
    cv::Mat f;
    if (!cap.read(f)) break;
    ++frames_checked;

    int fx, fy; double fc;
    if (detect_logo_in_frame(f, ts.template_img, ts.region, ts.threshold, fx, fy, fc)) {
      pts.push_back({ff, fx, fy});
      if (static_cast<int>(pts.size()) > FINE_HISTORY)
        pts.erase(pts.begin());

      if (verbose)
        std::cout << "    [" << label << "] frame " << ff
                  << " pos=(" << fx << "," << fy << ")\n";

      // Check after every new point — stop as soon as confirmed
      MoveResult r = classify_positions(pts, motion_threshold);
      if (r.confirmed) return r;
    }
  }
  return MoveResult{};  // not confirmed
}


// ── Usage ────────────────────────────────────────────────────────────────────
static void print_usage(const char* prog)
{
  std::cout
    << "Usage: " << prog << " --video PATH --config PATH [OPTIONS]\n\n"
    << "Detects moving logos using the same classifier as batch-delogo:\n"
    << "  dist >= motion-threshold on 2 consecutive coarse samples = MOVING.\n\n"
    << "Options:\n"
    << "  --video PATH              Video file to check\n"
    << "  --config PATH             Path to video_layouts.json\n"
    << "  --sample-interval N       Coarse scan: every Nth frame (default: 30)\n"
    << "  --motion-threshold PX     Per-step displacement to flag moving (default: 12)\n"
    << "  --fine-step N             Step for fine scan on logo-loss (default: 5)\n"
    << "  --verbose                 Print per-frame info\n"
    << "  --help                    Show this message\n"
    << std::endl;
}


// ── Print result ─────────────────────────────────────────────────────────────
static void print_found(const std::string& logo_name, const MoveResult& r,
                         double fps, double scan_secs, int frames_checked)
{
  double ts_sec = (fps > 0) ? r.last_pos.frame / fps : 0;
  int vm = static_cast<int>(ts_sec) / 60;
  int vs = static_cast<int>(ts_sec) % 60;
  std::cout << "\nMOVING LOGO DETECTED: true\n"
            << "  Logo      : " << logo_name << "\n"
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
  double motion_threshold   = 12.0;   // mirrors batch-delogo motion_step_threshold
  int    fine_step          = 5;
  bool   verbose            = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if      (arg == "--help" || arg == "-h")                { print_usage(argv[0]); return 0; }
    else if (arg == "--video"            && i+1 < argc)     video_path        = argv[++i];
    else if (arg == "--config"           && i+1 < argc)     config_path       = argv[++i];
    else if (arg == "--sample-interval"  && i+1 < argc)     sample_interval   = std::atoi(argv[++i]);
    else if (arg == "--motion-threshold" && i+1 < argc)     motion_threshold  = std::atof(argv[++i]);
    else if (arg == "--fine-step"        && i+1 < argc)     fine_step         = std::atoi(argv[++i]);
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

  // ── Open video ─────────────────────────────────────────────────────────
  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) {
    std::cerr << "Error: Cannot open video " << video_path << "\n";
    return 2;
  }

  int    frame_w     = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int    frame_h     = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  int    frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  double fps         = cap.get(cv::CAP_PROP_FPS);

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

  std::cout << "  Templates         : " << tracks.size() << "\n"
            << "  Sample interval   : " << sample_interval << " frames\n"
            << "  Motion threshold  : " << motion_threshold << " px/step\n"
            << "  Fine step         : " << fine_step << " frames (used on logo-loss)\n\n";

  // ── Coarse scan ────────────────────────────────────────────────────────
  auto t0 = std::chrono::steady_clock::now();
  int  frames_checked = 0;
  // Keep last (sample_interval+1) positions per track — enough for classification
  const int HISTORY_KEEP = 6;

  for (int fn = 0; fn < frame_count; fn += sample_interval) {
    cap.set(cv::CAP_PROP_POS_FRAMES, fn);
    cv::Mat frame;
    if (!cap.read(frame)) break;
    ++frames_checked;

    double time_sec = (fps > 0) ? fn / fps : 0;

    for (auto& ts : tracks) {
      int    x, y;
      double conf;
      bool found = detect_logo_in_frame(frame, ts.template_img, ts.region,
                                        ts.threshold, x, y, conf);

      if (found) {
        // ── verbose ────────────────────────────────────────────────
        if (verbose) {
          double step = 0;
          if (!ts.history.empty()) {
            double dx = x - ts.history.back().x;
            double dy = y - ts.history.back().y;
            step = std::sqrt(dx*dx + dy*dy);
          }
          std::cout << "  [" << ts.name << "] frame " << fn
                    << " (" << std::fixed << std::setprecision(1) << time_sec << "s)"
                    << " pos=(" << x << "," << y << ")"
                    << " step=" << std::setprecision(1) << step << "px\n";
        }

        ts.history.push_back({fn, x, y});
        // Trim history to HISTORY_KEEP entries
        if (static_cast<int>(ts.history.size()) > HISTORY_KEEP)
          ts.history.erase(ts.history.begin());

        // ── Classify with batch-delogo Phase-2 rule ───────────────
        MoveResult r = classify_positions(ts.history, motion_threshold);
        if (r.confirmed) {
          auto el = std::chrono::steady_clock::now() - t0;
          print_found(ts.name, r, fps,
                      std::chrono::duration<double>(el).count(), frames_checked);
          cap.release();
          return 0;  // EXIT: MOVING confirmed
        }

      } else {
        // Logo LOST — fine-scan the gap to catch fast (< 1 s) transitions
        if (!ts.history.empty()) {
          if (verbose)
            std::cout << "  [" << ts.name << "] frame " << fn
                      << " (" << std::setprecision(1) << time_sec << "s) LOST"
                      << " — fine-scanning...\n";

          int fine_start = std::max(0, ts.history.back().frame);
          int fine_end   = std::min(frame_count - 1, fn + sample_interval);

          MoveResult r = scan_range_rolling(cap, ts, fine_start, fine_end,
                                           fine_step, motion_threshold,
                                           frames_checked, verbose, "fine");
          if (r.confirmed) {
            auto el = std::chrono::steady_clock::now() - t0;
            print_found(ts.name, r, fps,
                        std::chrono::duration<double>(el).count(), frames_checked);
            cap.release();
            return 0;  // EXIT: MOVING confirmed via fine scan
          }

          if (verbose && !r.confirmed)
            std::cout << "    Fine scan: no movement confirmed\n";

          // Restore coarse position
          cap.set(cv::CAP_PROP_POS_FRAMES, fn);
        }
        // Reset history — logo absent, start fresh
        ts.history.clear();
      }
    }

    // Progress dot
    if (!verbose && frames_checked % 50 == 0) {
      int pct = fn * 100 / std::max(1, frame_count);
      std::cout << "  Scanned " << fn << "/" << frame_count
                << " (" << pct << "%)...\r" << std::flush;
    }
  }

  // ── No movement found ──────────────────────────────────────────────────
  auto elapsed = std::chrono::steady_clock::now() - t0;
  double secs  = std::chrono::duration<double>(elapsed).count();
  double total_sec = (fps > 0) ? frame_count / fps : 0;
  int vm = static_cast<int>(total_sec) / 60;
  int vs = static_cast<int>(total_sec) % 60;
  std::cout << "\nMOVING LOGO DETECTED: false\n"
            << "  Video duration : " << vm << "m " << vs << "s\n"
            << "  Scan time      : " << std::fixed << std::setprecision(1)
            << secs << "s (" << frames_checked << " frames checked)\n"
            << "  No movement above " << motion_threshold << "px/step detected.\n";
  cap.release();
  return 0;
}
