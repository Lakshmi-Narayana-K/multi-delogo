#!/usr/bin/env python3
"""
Round-trip: presigned GET download (same idea as pipeline.py) + upload_file upload.

Examples:
  python3 s3_roundtrip_test.py \\
    --source s3://src-bucket/path/video.mp4 \\
    --dest s3://dst-bucket/prefix/video_copy.mp4 \\
    --dl-key-id AKIA... --dl-secret '...' --dl-region ap-south-1

  # Same AWS identity for both (if it can read source and write dest):
  AWS_PROFILE=my-profile python3 s3_roundtrip_test.py \\
    --source s3://... --dest s3://...
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile

import boto3
import urllib.request


def parse_s3_url(url: str) -> tuple[str, str]:
    url = url.replace("s3://", "")
    bucket, _, key = url.partition("/")
    return bucket, key


def build_s3_download_client(
    key_id: str | None,
    secret: str | None,
    token: str | None,
    region: str,
) -> boto3.client:
    s3 = boto3.client("s3")
    if key_id and secret:
        return boto3.client(
            "s3",
            aws_access_key_id=key_id,
            aws_secret_access_key=secret,
            aws_session_token=token,
            region_name=region,
            config=boto3.session.Config(signature_version="s3v4"),
        )
    return s3


def main() -> None:
    p = argparse.ArgumentParser(description="S3 presigned download + upload round-trip")
    p.add_argument("--source", required=True, help="s3://bucket/key to read")
    p.add_argument("--dest", required=True, help="s3://bucket/key to write")
    p.add_argument("--dl-key-id", default=None)
    p.add_argument("--dl-secret", default=None)
    p.add_argument("--dl-token", default=None)
    p.add_argument("--dl-region", default="ap-south-1")
    args = p.parse_args()

    s3 = boto3.client("s3")
    s3_download = build_s3_download_client(
        args.dl_key_id,
        args.dl_secret,
        args.dl_token,
        args.dl_region,
    )

    src_bucket, src_key = parse_s3_url(args.source)
    dst_bucket, dst_key = parse_s3_url(args.dest)
    filename = src_key.split("/")[-1] or "object.bin"

    with tempfile.TemporaryDirectory(prefix="s3_roundtrip_") as tmp:
        local_path = os.path.join(tmp, filename)

        print("1) Presign + download")
        presigned_url = s3_download.generate_presigned_url(
            "get_object",
            Params={"Bucket": src_bucket, "Key": src_key},
            ExpiresIn=604800,
        )
        print("   URL:", presigned_url[:100] + "...")
        urllib.request.urlretrieve(presigned_url, local_path)
        print("   Downloaded", os.path.getsize(local_path), "bytes")

        print("2) upload_file")
        s3.upload_file(local_path, dst_bucket, dst_key)
        print(f"   OK s3://{dst_bucket}/{dst_key}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("ERROR:", e, file=sys.stderr)
        sys.exit(1)