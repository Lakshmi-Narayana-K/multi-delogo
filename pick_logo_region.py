#!/usr/bin/env python3
"""
Two-step logo region picker.

STEP 1 — Extract and open the frame:
  python3 pick_logo_region.py <video> <time_sec>

  Opens the frame in eog. Hover over the logo corners to read pixel coordinates
  from the status bar (shown as "X: NNN  Y: NNN").

STEP 2 — Crop and save using the coordinates you noted:
  python3 pick_logo_region.py <video> <time_sec> <x1> <y1> <x2> <y2> <output_png>

Examples:
  python3 pick_logo_region.py videos_to_process/215s.mp4 7
  python3 pick_logo_region.py videos_to_process/215s.mp4 7 1040 625 1280 710 reference_images/nxt+ib-corner_720.png
"""

import cv2
import sys
import subprocess
import os

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    video_path = sys.argv[1]
    time_sec   = float(sys.argv[2])

    # Extract frame
    cap = cv2.VideoCapture(video_path)
    fps = cap.get(cv2.CAP_PROP_FPS)
    cap.set(cv2.CAP_PROP_POS_FRAMES, int(time_sec * fps))
    ret, frame = cap.read()
    cap.release()

    if not ret:
        print("ERROR: Failed to read frame from video")
        sys.exit(1)

    frame_path = "/tmp/pick_frame.png"
    cv2.imwrite(frame_path, frame)
    print(f"Frame saved: {frame_path}  ({frame.shape[1]}x{frame.shape[0]})")

    # Step 1 only — open image in browser for coordinate picking
    if len(sys.argv) == 3:
        import http.server
        import threading
        import base64

        with open(frame_path, "rb") as f:
            img_b64 = base64.b64encode(f.read()).decode()

        html = f"""<!DOCTYPE html>
<html>
<head><title>Pick Logo Region</title>
<style>
  body {{ margin:0; background:#222; display:flex; flex-direction:column; align-items:center; }}
  h2 {{ color:#fff; font-family:monospace; margin:8px; }}
  #info {{ color:#0f0; font-family:monospace; font-size:16px; margin:6px; }}
  #coords {{ color:#ff0; font-family:monospace; font-size:14px; margin:4px; min-height:20px; }}
  canvas {{ cursor:crosshair; border:2px solid #555; }}
</style>
</head>
<body>
<h2>Click TOP-LEFT then BOTTOM-RIGHT of the logo</h2>
<div id="info">Clicks: 0/2 — move mouse over image to see coordinates</div>
<div id="coords"></div>
<canvas id="c"></canvas>
<script>
  const img = new Image();
  img.src = "data:image/png;base64,{img_b64}";
  img.onload = () => {{
    const c = document.getElementById('c');
    c.width = img.width; c.height = img.height;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0);

    let pts = [];
    c.addEventListener('mousemove', e => {{
      const r = c.getBoundingClientRect();
      const x = Math.round(e.clientX - r.left);
      const y = Math.round(e.clientY - r.top);
      document.getElementById('coords').textContent = 'Mouse: X=' + x + '  Y=' + y;
    }});
    c.addEventListener('click', e => {{
      const r = c.getBoundingClientRect();
      const x = Math.round(e.clientX - r.left);
      const y = Math.round(e.clientY - r.top);
      pts.push([x, y]);
      ctx.fillStyle = 'red';
      ctx.beginPath(); ctx.arc(x, y, 5, 0, 2*Math.PI); ctx.fill();
      if (pts.length === 1) {{
        document.getElementById('info').textContent = 'TOP-LEFT: (' + x + ', ' + y + ') — now click BOTTOM-RIGHT';
        ctx.fillStyle='red'; ctx.font='14px monospace';
        ctx.fillText('TL('+x+','+y+')', x+6, y-4);
      }} else if (pts.length === 2) {{
        const [x1,y1] = pts[0];
        ctx.strokeStyle='lime'; ctx.lineWidth=2;
        ctx.strokeRect(x1, y1, x-x1, y-y1);
        ctx.fillStyle='lime'; ctx.font='14px monospace';
        ctx.fillText('BR('+x+','+y+')', x+6, y+14);
        const cmd = `python3 pick_logo_region.py {video_path} {time_sec} ${{x1}} ${{y1}} ${{x}} ${{y}} <output.png>`;
        document.getElementById('info').innerHTML =
          '<span style="color:#0ff">DONE! Run this command:</span><br>' + cmd;
        document.getElementById('coords').textContent =
          'Region: x='+x1+' y='+y1+' w='+(x-x1)+' h='+(y-y1);
      }}
    }});
  }};
</script>
</body></html>"""

        html_path = "/tmp/pick_logo.html"
        with open(html_path, "w") as f:
            f.write(html)

        print(f"Opening browser at http://localhost:8765")
        print("Click TOP-LEFT then BOTTOM-RIGHT of the logo.")
        print("The command to run will appear on screen after 2 clicks.")
        print("Press Ctrl+C here when done.")

        subprocess.Popen(["xdg-open", "http://localhost:8765"])

        os.chdir("/tmp")
        class Handler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *a): pass
            def do_GET(self):
                if self.path == "/":
                    self.send_response(200)
                    self.send_header("Content-type", "text/html")
                    self.end_headers()
                    self.wfile.write(html.encode())
                else:
                    super().do_GET()
        httpd = http.server.HTTPServer(("", 8765), Handler)
        httpd.serve_forever()
        return

    # Step 2 — crop with provided coordinates
    if len(sys.argv) == 8:
        x1, y1, x2, y2 = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
        output_path = sys.argv[7]

        w, h = x2 - x1, y2 - y1
        crop = frame[y1:y2, x1:x2]
        cv2.imwrite(output_path, crop)

        print(f"Cropped region: x={x1}, y={y1}, w={w}, h={h}")
        print(f"Saved: {output_path}  ({w}x{h})")

        # # Quick confidence test
        # print()
        # print("Running confidence test on same video...")
        # template = cv2.imread(output_path)
        # import numpy as np
        # SCALES = [0.9, 0.95, 1.0, 1.05, 1.1]
        # cap = cv2.VideoCapture(video_path)
        # total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        # fps2  = cap.get(cv2.CAP_PROP_FPS)
        # best_conf, best_frame_num = 0, -1
        # for fn in range(0, total, 30):
        #     cap.set(cv2.CAP_PROP_POS_FRAMES, fn)
        #     ret2, f = cap.read()
        #     if not ret2: break
        #     fh2, fw2 = f.shape[:2]
        #     bv = -1.0
        #     for s in SCALES:
        #         sw, sh = int(w*s), int(h*s)
        #         if sw < 5 or sh < 5 or sw > fw2 or sh > fh2: continue
        #         scaled = cv2.resize(template, (sw, sh), cv2.INTER_LINEAR)
        #         res = cv2.matchTemplate(f, scaled, cv2.TM_CCOEFF_NORMED)
        #         _, mv, _, _ = cv2.minMaxLoc(res)
        #         if mv > bv: bv = mv
        #     sec = fn / fps2
        #     mark = " ✓ DETECTED" if bv >= 0.75 else ""
        #     print(f"  frame {fn:4d}  {int(sec//60)}m{int(sec%60):02d}s  conf={bv:.3f}{mark}")
        #     if bv > best_conf: best_conf, best_frame_num = bv, fn
        # cap.release()
        # print(f"\nBest confidence: {best_conf:.3f} at frame {best_frame_num}")
        return

    print("ERROR: Wrong number of arguments.")
    print(__doc__)


if __name__ == "__main__":
    main()
