"""
ECS Batch Delogo Pipeline
--------------------------
Reads a CSV of S3 video URLs, processes each with batch-delogo,
uploads results back to S3, writes an output mapping CSV.

Required env vars:
  INPUT_CSV_S3   - s3://bucket/path/input.csv
  OUTPUT_CSV_S3  - s3://bucket/path/output.csv

Optional env vars:
  OUTPUT_PREFIX  - legacy; processed videos are uploaded next to each source
                    object (same bucket + key directory, *_ibhlr filename).
  AWS_DEFAULT_REGION / AWS_REGION - optional; improves S3 client region selection.

Credentials:
  Use the default AWS credential chain (e.g. EC2 instance IAM role). Do not set
  long-lived access keys in the environment for this pipeline.

Optional env vars for parallel execution:
  TOTAL_WORKERS  - total number of parallel instances (default: 1)
  WORKER_INDEX   - this instance's index, 0-based (default: 0)

  Each instance processes rows where: row_index % TOTAL_WORKERS == WORKER_INDEX
  Output CSV per worker: OUTPUT_CSV_S3 base name gets suffix _worker<N>.csv

Example (4 EC2 instances):
  Instance 0: TOTAL_WORKERS=4 WORKER_INDEX=0 python3 pipeline.py
  Instance 1: TOTAL_WORKERS=4 WORKER_INDEX=1 python3 pipeline.py
  Instance 2: TOTAL_WORKERS=4 WORKER_INDEX=2 python3 pipeline.py
  Instance 3: TOTAL_WORKERS=4 WORKER_INDEX=3 python3 pipeline.py
"""

import os
import io
import csv
import subprocess
import shutil
import sys
import boto3
import urllib.request

INPUT_CSV_S3  = os.environ["INPUT_CSV_S3"]
OUTPUT_PREFIX = os.environ.get("OUTPUT_PREFIX", "").rstrip("/")  # optional; unused for video keys
OUTPUT_CSV_S3 = os.environ["OUTPUT_CSV_S3"]

TOTAL_WORKERS = int(os.environ.get("TOTAL_WORKERS", "1"))
WORKER_INDEX  = int(os.environ.get("WORKER_INDEX",  "0"))

BINARY        = "./src/batch-delogo/batch-delogo"
CONFIG        = "./video_layouts.json"
INPUT_DIR     = f"./videos_to_process{WORKER_INDEX}"
OUTPUT_DIR    = f"./processed_videos{WORKER_INDEX}"
REPORT_PATH   = f"./report_worker{WORKER_INDEX}.csv"

_s3_kw = {"config": boto3.session.Config(signature_version="s3v4")}
_s3_region = os.environ.get("AWS_DEFAULT_REGION") or os.environ.get("AWS_REGION")
if _s3_region:
    _s3_kw["region_name"] = _s3_region
s3 = boto3.client("s3", **_s3_kw)


def worker_output_csv_s3():
    """Each worker writes to its own output CSV to avoid conflicts."""
    if TOTAL_WORKERS == 1:
        return OUTPUT_CSV_S3
    base, _, ext = OUTPUT_CSV_S3.rpartition(".")
    return f"{base}_worker{WORKER_INDEX}.{ext}"


def parse_s3_url(url):
    """s3://bucket/key/path  ->  (bucket, key)"""
    url = url.replace("s3://", "")
    bucket, _, key = url.partition("/")
    return bucket, key


OUTPUT_BUCKET = "nxtwave-common-media-static"


def output_s3_url_same_path_as_source(source_s3_url, output_filename):
    """
    s3://src-bucket/a/b/c/video.mp4 -> s3://nxtwave-common-media-static/src-bucket/a/b/c/video_ibhlr.mp4
    Preserves full source path under OUTPUT_BUCKET with source bucket name as top-level prefix.
    """
    src_bucket, src_key = parse_s3_url(source_s3_url.strip())
    key_dir, sep, _ = src_key.rpartition("/")
    if sep:
        new_key = f"{src_bucket}/{key_dir}/{output_filename}"
    else:
        new_key = f"{src_bucket}/{output_filename}"
    return f"s3://{OUTPUT_BUCKET}/{new_key}"


def download_from_s3(s3_url, dest_path):
    bucket, key = parse_s3_url(s3_url)
    presigned_url = s3.generate_presigned_url(
        "get_object",
        Params={"Bucket": bucket, "Key": key},
        ExpiresIn=604800,
    )
    print(f"  Presigned URL: {presigned_url}")
    urllib.request.urlretrieve(presigned_url, dest_path)


def upload_to_s3(local_path, s3_url):
    bucket, key = parse_s3_url(s3_url)
    s3.upload_file(local_path, bucket, key)


def read_input_csv():
    bucket, key = parse_s3_url(INPUT_CSV_S3)
    obj = s3.get_object(Bucket=bucket, Key=key)
    content = obj["Body"].read().decode("utf-8")
    reader = csv.DictReader(io.StringIO(content))
    return list(reader)


def write_output_csv(rows, dest_s3_url=None):
    if dest_s3_url is None:
        dest_s3_url = worker_output_csv_s3()
    buf = io.StringIO()
    fieldnames = ["row_index", "old_s3_url", "new_s3_url", "status", "moving_logos"]
    writer = csv.DictWriter(buf, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)
    bucket, key = parse_s3_url(dest_s3_url)
    s3.put_object(Bucket=bucket, Key=key, Body=buf.getvalue().encode("utf-8"))
    print(f"  [Worker {WORKER_INDEX}] Progress CSV -> {dest_s3_url}")


def process_video(old_s3_url):
    filename = old_s3_url.split("/")[-1]
    input_path = os.path.join(INPUT_DIR, filename)

    # derive expected output filename (batch-delogo appends _processed before extension)
    name, ext = os.path.splitext(filename)
    output_filename = f"{name}_ibhlr{ext}"
    output_path = os.path.join(OUTPUT_DIR, output_filename)

    new_s3_url = output_s3_url_same_path_as_source(old_s3_url, output_filename)

    try:
        # 1. Download from S3
        print(f"  Downloading {old_s3_url} ...")
        download_from_s3(old_s3_url, input_path)

        # 2. Run batch-delogo
        print(f"  Running batch-delogo on {filename} ...")
        result = subprocess.run(
            [
                BINARY,
                "--input-folder",  INPUT_DIR,
                "--output-folder", OUTPUT_DIR,
                "--config",        CONFIG,
                "--auto-detect",
                "--report",        REPORT_PATH,
            ],
            capture_output=False,
            timeout=36000,  # 10 hour hard limit per video
        )

        if result.returncode != 0:
            raise RuntimeError(f"batch-delogo exited with code {result.returncode}")

        if not os.path.exists(output_path):
            raise FileNotFoundError(f"Expected output not found: {output_path}")

        # 3. Parse moving_logos from report CSV (one row per run)
        moving_logos = ""
        if os.path.exists(REPORT_PATH):
            with open(REPORT_PATH, newline="") as rf:
                for report_row in csv.DictReader(rf):
                    moving_logos = report_row.get("moving_logos", "")
                    break  # only one row expected

        # 4. Upload processed video to S3
        print(f"  Uploading to {new_s3_url} ...")
        upload_to_s3(output_path, new_s3_url)

        return {"old_s3_url": old_s3_url, "new_s3_url": new_s3_url, "status": "success",
                "moving_logos": moving_logos}

    except Exception as e:
        print(f"  ERROR processing {filename}: {e}", file=sys.stderr)
        return {"old_s3_url": old_s3_url, "new_s3_url": "", "status": f"error: {e}",
                "moving_logos": ""}

    finally:
        # 5. Always clean up local files to free disk
        if os.path.exists(input_path):
            os.remove(input_path)
            print(f"  Deleted local input: {input_path}")
        if os.path.exists(output_path):
            os.remove(output_path)
            print(f"  Deleted local output: {output_path}")
        if os.path.exists(REPORT_PATH):
            os.remove(REPORT_PATH)


def main():
    os.makedirs(INPUT_DIR,  exist_ok=True)
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    print(f"[Worker {WORKER_INDEX}/{TOTAL_WORKERS}] Reading input CSV from {INPUT_CSV_S3}")
    all_rows = read_input_csv()
    print(f"[Worker {WORKER_INDEX}/{TOTAL_WORKERS}] Total rows in CSV: {len(all_rows)}")

    # Each worker handles rows where row_index % TOTAL_WORKERS == WORKER_INDEX
    my_rows = [
        (row_index, row)
        for row_index, row in enumerate(all_rows)
        if row_index % TOTAL_WORKERS == WORKER_INDEX
    ]

    print(f"[Worker {WORKER_INDEX}/{TOTAL_WORKERS}] Assigned {len(my_rows)} video(s): "
          f"rows {[i for i, _ in my_rows]}")

    if not my_rows:
        print(f"[Worker {WORKER_INDEX}/{TOTAL_WORKERS}] No rows assigned, exiting.")
        return

    out_csv = worker_output_csv_s3()
    print(f"[Worker {WORKER_INDEX}/{TOTAL_WORKERS}] Output CSV -> {out_csv}")

    results = []
    for position, (row_index, row) in enumerate(my_rows, 1):
        old_url = row["s3_url"].strip()
        print(f"\n[Worker {WORKER_INDEX}] [{position}/{len(my_rows)}] row#{row_index} {old_url}")
        result = process_video(old_url)
        result["row_index"] = row_index
        results.append(result)

        # Write output CSV after every video so progress is saved even if the instance crashes
        write_output_csv(results, out_csv)

    success = sum(1 for r in results if r["status"] == "success")
    errors  = len(results) - success
    print(f"\n[Worker {WORKER_INDEX}/{TOTAL_WORKERS}] Done. Success: {success}  Errors: {errors}")
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
