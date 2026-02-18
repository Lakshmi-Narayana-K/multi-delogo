/*
 * detect-moving: Quick check whether a video contains moving logos.
 *
 * Two-phase detection:
 *   Phase 1 (coarse): Samples every Nth frame.  When a big position jump OR
 *                      a logo disappearance is detected, triggers Phase 2.
 *   Phase 2 (fine):    Reads every few frames in the gap and checks for
 *                      consistent linear movement.  If confirmed, prints
 *                      "true" and exits immediately.
 *
 * A logo is "moving" only when it shows real linear movement (same direction
 * across multiple frames, significant total displacement).
 *
 * Exit codes: 0 = normal (result printed), 2 = error.
 *
 * Usage:
 *   detect-moving --video PATH --config PATH [OPTIONS]
 */

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "filter-generator/VideoLayoutConfig.hpp"


// ── Template matching (single-scale) ────────────────────────────────────────
static bool detect_logo_in_frame(const cv::Mat& frame,
                                 const cv::Mat& template_img,
                                 const fg::SearchRegion& region,
                                 double threshold,
                                 int& found_x, int& found_y,
                                 double& confidence)
{
  int roi_x = std::max(0, region.x);
  int roi_y = std::max(0, region.y);
  int roi_w = std::min(region.width, frame.cols - roi_x);
  int roi_h = std::min(region.height, frame.rows - roi_y);
  if (roi_w <= 0 || roi_h <= 0) return false;

  cv::Rect roi(roi_x, roi_y, roi_w, roi_h);
  cv::Mat search_area = frame(roi);

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
struct PosSample {
  int frame;
  int x;
  int y;
};


// ── Per-detection tracking state ────────────────────────────────────────────
struct TrackState {
  std::string      name;
  cv::Mat          template_img;
  fg::SearchRegion region;
  double           threshold;

  bool has_prev   = false;
  int  prev_frame = 0;
  int  prev_x     = 0;
  int  prev_y     = 0;
};


// ── Check if a sequence of positions is consistent linear movement ──────────
// Requires:
//   - at least 3 points
//   - net displacement >= min_shift
//   - every step moves in the same direction as net (dot > 0)
//   - every step has magnitude > 1px
static bool is_linear(const std::vector<PosSample>& pts, double min_shift,
                      double& net_dist, std::string& direction)
{
  if (pts.size() < 3) return false;

  double net_dx = pts.back().x - pts.front().x;
  double net_dy = pts.back().y - pts.front().y;
  net_dist = std::sqrt(net_dx * net_dx + net_dy * net_dy);
  if (net_dist < min_shift) return false;

  for (size_t i = 1; i < pts.size(); ++i) {
    double dx = pts[i].x - pts[i - 1].x;
    double dy = pts[i].y - pts[i - 1].y;
    double dot = dx * net_dx + dy * net_dy;
    if (dot <= 0) return false;
    double d = std::sqrt(dx * dx + dy * dy);
    if (d < 1.0) return false;
  }

  if (std::abs(net_dx) > std::abs(net_dy))
    direction = (net_dx > 0) ? "left-to-right" : "right-to-left";
  else
    direction = (net_dy > 0) ? "top-to-bottom" : "bottom-to-top";

  return true;
}


// ── Fine scan: read frames in a range and collect positions ─────────────────
static std::vector<PosSample> fine_scan(cv::VideoCapture& cap,
                                        const TrackState& ts,
                                        int start_frame, int end_frame,
                                        int step, int& frames_checked,
                                        bool verbose)
{
  std::vector<PosSample> pts;
  for (int ff = start_frame; ff <= end_frame; ff += step) {
    cap.set(cv::CAP_PROP_POS_FRAMES, ff);
    cv::Mat fframe;
    if (!cap.read(fframe)) break;
    ++frames_checked;

    int fx, fy;
    double fc;
    if (detect_logo_in_frame(fframe, ts.template_img, ts.region,
                             ts.threshold, fx, fy, fc)) {
      pts.push_back({ff, fx, fy});
      if (verbose) {
        std::cout << "    fine frame " << ff
                  << " pos=(" << fx << "," << fy << ")\n";
      }
    }
  }
  return pts;
}


// ── Usage ───────────────────────────────────────────────────────────────────
static void print_usage(const char* prog)
{
  std::cout
    << "Usage: " << prog << " --video PATH --config PATH [OPTIONS]\n\n"
    << "Quickly detect whether a video contains moving logos.\n"
    << "Detects real linear movement (e.g. sliding left-to-right), ignores jitter.\n\n"
    << "Options:\n"
    << "  --video PATH            Video file to check\n"
    << "  --config PATH           Path to video_layouts.json\n"
    << "  --sample-interval N     Coarse scan: every Nth frame (default: 30)\n"
    << "  --jump-threshold PX     Coarse jump that triggers fine scan (default: 30)\n"
    << "  --min-total-shift PX    Net displacement in fine scan to confirm (default: 50)\n"
    << "  --verbose               Print per-frame info\n"
    << "  --help                  Show this message\n"
    << std::endl;
}


// ── main ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
  std::string video_path;
  std::string config_path;
  int    sample_interval  = 30;
  double jump_threshold   = 30.0;
  double min_total_shift  = 50.0;
  bool   verbose          = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
    else if (arg == "--video"            && i + 1 < argc) video_path       = argv[++i];
    else if (arg == "--config"           && i + 1 < argc) config_path      = argv[++i];
    else if (arg == "--sample-interval"  && i + 1 < argc) sample_interval  = std::atoi(argv[++i]);
    else if (arg == "--jump-threshold"   && i + 1 < argc) jump_threshold   = std::atof(argv[++i]);
    else if (arg == "--min-total-shift"  && i + 1 < argc) min_total_shift  = std::atof(argv[++i]);
    else if (arg == "--verbose") verbose = true;
    else { std::cerr << "Unknown option: " << arg << "\n"; print_usage(argv[0]); return 2; }
  }

  if (video_path.empty() || config_path.empty()) {
    std::cerr << "Error: --video and --config are required.\n";
    print_usage(argv[0]);
    return 2;
  }

  // ── Load config ───────────────────────────────────────────────────────
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

  // ── Open video ────────────────────────────────────────────────────────
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

  // ── Build tracking state per detection ────────────────────────────────
  std::vector<TrackState> tracks;
  for (const auto& det : layout->detections) {
    std::string ref = layout_mgr.get_reference_path(det.reference_image);
    cv::Mat tmpl = cv::imread(ref);
    if (tmpl.empty()) {
      std::cerr << "  Warning: cannot load " << ref << "\n";
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

  std::cout << "  Templates       : " << tracks.size() << "\n"
            << "  Sample interval : " << sample_interval << " frames\n"
            << "  Jump threshold  : " << jump_threshold << " px  (triggers fine scan)\n"
            << "  Min total shift : " << min_total_shift << " px  (confirms movement)\n\n";

  // ── Helper: attempt fine scan and check for linear movement ───────────
  auto try_fine_scan = [&](TrackState& ts, int fine_start, int fine_end,
                           int& frames_checked, const char* reason) -> bool
  {
    const int fine_step = 5;  // every 5th frame

    if (verbose) {
      std::cout << "    -> " << reason
                << ", fine scanning frames " << fine_start << "-" << fine_end << "...\n";
    }

    auto pts = fine_scan(cap, ts, fine_start, fine_end, fine_step,
                         frames_checked, verbose);

    double net_dist = 0;
    std::string direction;
    if (is_linear(pts, min_total_shift, net_dist, direction)) {
      double ts_sec = (fps > 0) ? pts.back().frame / fps : 0;
      int vm = static_cast<int>(ts_sec) / 60;
      int vs = static_cast<int>(ts_sec) % 60;

      std::cout << "\nMOVING LOGO DETECTED: true\n"
                << "  Logo      : " << ts.name << "\n"
                << "  Direction : " << direction << "\n"
                << "  Frames    : " << pts.front().frame
                << " -> " << pts.back().frame
                << "  (video time " << vm << "m " << vs << "s)\n"
                << "  Start pos : (" << pts.front().x << ", " << pts.front().y << ")\n"
                << "  End pos   : (" << pts.back().x << ", " << pts.back().y << ")\n"
                << "  Net shift : " << std::fixed << std::setprecision(1)
                << net_dist << " px\n";
      return true;
    }

    if (verbose) {
      std::cout << "    Fine scan: not confirmed linear ("
                << pts.size() << " pts, net="
                << std::fixed << std::setprecision(1) << net_dist << "px)\n";
    }
    return false;
  };

  // ── Phase 1: Coarse scan ──────────────────────────────────────────────
  auto t0 = std::chrono::steady_clock::now();
  int  frames_checked = 0;

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
        if (verbose) {
          double step = 0;
          if (ts.has_prev) {
            double dx = x - ts.prev_x, dy = y - ts.prev_y;
            step = std::sqrt(dx * dx + dy * dy);
          }
          std::cout << "  [" << ts.name << "] frame " << fn
                    << " (" << std::fixed << std::setprecision(1) << time_sec << "s)"
                    << " pos=(" << x << "," << y << ")"
                    << " step=" << step << "px\n";
        }

        // Case 1: Big jump between two consecutive found positions
        if (ts.has_prev) {
          double dx = x - ts.prev_x;
          double dy = y - ts.prev_y;
          double jump = std::sqrt(dx * dx + dy * dy);

          if (jump >= jump_threshold) {
            int fine_start = ts.prev_frame;
            int fine_end   = std::min(frame_count - 1, fn + sample_interval);

            if (try_fine_scan(ts, fine_start, fine_end, frames_checked, "Big jump")) {
              auto el = std::chrono::steady_clock::now() - t0;
              std::cout << "  Scan time : " << std::setprecision(1)
                        << std::chrono::duration<double>(el).count() << "s ("
                        << frames_checked << " frames checked)\n";
              cap.release();
              return 0;
            }
            cap.set(cv::CAP_PROP_POS_FRAMES, fn);  // restore position
          }
        }

        ts.prev_frame = fn;
        ts.prev_x     = x;
        ts.prev_y     = y;
        ts.has_prev   = true;

      } else {
        // Case 2: Logo was being tracked and now LOST
        //         Fine-scan the gap to see if it slid away before disappearing
        if (ts.has_prev) {
          if (verbose) {
            std::cout << "  [" << ts.name << "] frame " << fn
                      << " (" << std::setprecision(1) << time_sec << "s) LOST\n";
          }

          int fine_start = std::max(0, ts.prev_frame - sample_interval);
          int fine_end   = std::min(frame_count - 1, fn + sample_interval);

          if (try_fine_scan(ts, fine_start, fine_end, frames_checked, "Logo lost")) {
            auto el = std::chrono::steady_clock::now() - t0;
            std::cout << "  Scan time : " << std::setprecision(1)
                      << std::chrono::duration<double>(el).count() << "s ("
                      << frames_checked << " frames checked)\n";
            cap.release();
            return 0;
          }
          cap.set(cv::CAP_PROP_POS_FRAMES, fn);  // restore position
        }
        ts.has_prev = false;
      }
    }

    // Progress
    if (!verbose && frames_checked % 50 == 0) {
      int pct = fn * 100 / std::max(1, frame_count);
      std::cout << "  Scanned " << fn << "/" << frame_count
                << " (" << pct << "%)...\r" << std::flush;
    }
  }

  // ── No movement found ─────────────────────────────────────────────────
  auto elapsed = std::chrono::steady_clock::now() - t0;
  double secs  = std::chrono::duration<double>(elapsed).count();
  double total_sec = (fps > 0) ? frame_count / fps : 0;
  int vm = static_cast<int>(total_sec) / 60;
  int vs = static_cast<int>(total_sec) % 60;
  std::cout << "\nMOVING LOGO DETECTED: false\n"
            << "  Video duration: " << vm << "m " << vs << "s\n"
            << "  Scan time : " << std::fixed << std::setprecision(1)
            << secs << "s (" << frames_checked << " frames checked)\n"
            << "  No linear movement above " << min_total_shift << "px detected.\n";
  cap.release();
  return 0;
}
