import boto3, csv, io

s3 = boto3.client('s3', region_name='ap-south-1')
BUCKET        = "nw-studio-nkb-uploads"
PREFIX        = "ib_logo_processed_test"
WORKER_START  = 0     # inclusive — change per batch
WORKER_END    = 399  # inclusive — change per batch

all_rows = []
missing  = []

for i in range(WORKER_START, WORKER_END + 1):
    key = f"{PREFIX}/output_worker{i}.csv"
    try:
        obj  = s3.get_object(Bucket=BUCKET, Key=key)
        data = obj["Body"].read().decode("utf-8")
        for row in csv.DictReader(io.StringIO(data)):
            all_rows.append(row)
    except s3.exceptions.NoSuchKey:
        missing.append(i)

# Sort by original row index
all_rows.sort(key=lambda r: int(r["row_index"]))

# Write final CSV
with open("final_output.csv", "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=["row_index", "old_s3_url", "new_s3_url", "status", "moving_logos"])
    writer.writeheader()
    writer.writerows(all_rows)

print(f"Written {len(all_rows)} rows to final_output.csv")
if missing:
    print(f"WARNING: {len(missing)} workers had no output: {missing[:20]}...")