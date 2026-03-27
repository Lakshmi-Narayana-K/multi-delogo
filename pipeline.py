"""
ECS Batch Delogo Pipeline
--------------------------
Reads a CSV of S3 video URLs, processes each with batch-delogo,
uploads results back to S3, writes an output mapping CSV.

Required env vars:
  INPUT_CSV_S3   - s3://bucket/path/input.csv
  OUTPUT_PREFIX  - s3://bucket/processed/  (trailing slash)
  OUTPUT_CSV_S3  - s3://bucket/path/output.csv
"""

import os
import io
import csv
import subprocess
import shutil
import sys
import boto3

INPUT_CSV_S3  = os.environ["INPUT_CSV_S3"]
OUTPUT_PREFIX = os.environ["OUTPUT_PREFIX"].rstrip("/")
OUTPUT_CSV_S3 = os.environ["OUTPUT_CSV_S3"]

BINARY        = "./batch-delogo"
CONFIG        = "./video_layouts.json"
INPUT_DIR     = "./videos_to_process"
OUTPUT_DIR    = "./processed_videos"

s3 = boto3.client("s3")


def parse_s3_url(url):
    """s3://bucket/key/path  ->  (bucket, key)"""
    url = url.replace("s3://", "")
    bucket, _, key = url.partition("/")
    return bucket, key


def download_from_s3(s3_url, dest_path):
    bucket, key = parse_s3_url(s3_url)
    s3.download_file(bucket, key, dest_path)


def upload_to_s3(local_path, s3_url):
    bucket, key = parse_s3_url(s3_url)
    s3.upload_file(local_path, bucket, key)


def read_input_csv():
    bucket, key = parse_s3_url(INPUT_CSV_S3)
    obj = s3.get_object(Bucket=bucket, Key=key)
    content = obj["Body"].read().decode("utf-8")
    reader = csv.DictReader(io.StringIO(content))
    return list(reader)


def write_output_csv(rows):
    buf = io.StringIO()
    fieldnames = ["old_s3_url", "new_s3_url", "status"]
    writer = csv.DictWriter(buf, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)
    bucket, key = parse_s3_url(OUTPUT_CSV_S3)
    s3.put_object(Bucket=bucket, Key=key, Body=buf.getvalue().encode("utf-8"))
    print(f"Output CSV written to {OUTPUT_CSV_S3}")


def process_video(old_s3_url):
    filename = old_s3_url.split("/")[-1]
    input_path = os.path.join(INPUT_DIR, filename)

    # derive expected output filename (batch-delogo appends _processed before extension)
    name, ext = os.path.splitext(filename)
    output_filename = f"{name}_processed{ext}"
    output_path = os.path.join(OUTPUT_DIR, output_filename)

    new_s3_url = f"{OUTPUT_PREFIX}/{output_filename}"

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
            ],
            capture_output=False,
            timeout=7200,  # 2 hour hard limit per video
        )

        if result.returncode != 0:
            raise RuntimeError(f"batch-delogo exited with code {result.returncode}")

        if not os.path.exists(output_path):
            raise FileNotFoundError(f"Expected output not found: {output_path}")

        # 3. Upload processed video to S3
        print(f"  Uploading to {new_s3_url} ...")
        upload_to_s3(output_path, new_s3_url)

        return {"old_s3_url": old_s3_url, "new_s3_url": new_s3_url, "status": "success"}

    except Exception as e:
        print(f"  ERROR processing {filename}: {e}", file=sys.stderr)
        return {"old_s3_url": old_s3_url, "new_s3_url": "", "status": f"error: {e}"}

    finally:
        # 4. Always clean up local files to free disk
        if os.path.exists(input_path):
            os.remove(input_path)
            print(f"  Deleted local input: {input_path}")
        if os.path.exists(output_path):
            os.remove(output_path)
            print(f"  Deleted local output: {output_path}")


def main():
    os.makedirs(INPUT_DIR,  exist_ok=True)
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    print(f"Reading input CSV from {INPUT_CSV_S3}")
    rows = read_input_csv()
    print(f"Found {len(rows)} video(s) to process")

    # Input CSV must have a column named 's3_url'
    results = []
    for i, row in enumerate(rows, 1):
        old_url = row["s3_url"].strip()
        print(f"\n[{i}/{len(rows)}] {old_url}")
        result = process_video(old_url)
        results.append(result)

        # Write output CSV after every video so progress is saved even if the task crashes
        write_output_csv(results)

    # Summary
    success = sum(1 for r in results if r["status"] == "success")
    errors  = len(results) - success
    print(f"\nDone. Success: {success}  Errors: {errors}")
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
