#!/bin/bash
# Cloud-init runs this as root. Re-exec the real work as ubuntu so HOME and
# boto3's default credential chain (EC2 instance IAM role via IMDS) match SSH.
set -e
exec > /tmp/delogo-root-wrapper.log 2>&1

sudo -u ubuntu -H bash <<'UBUNTU_SCRIPT'
set -e

# --- Ensure self-terminate always runs even if script aborts early ---
terminate_self() {
    echo "terminate_self triggered. Terminating instance..."
    set +e
    IMDS_TOKEN=$(curl -s -X PUT "http://169.254.169.254/latest/api/token" -H "X-aws-ec2-metadata-token-ttl-seconds: 21600")
    INSTANCE_ID=$(curl -s -H "X-aws-ec2-metadata-token: $IMDS_TOKEN" http://169.254.169.254/latest/meta-data/instance-id)
    REGION=$(curl -s -H "X-aws-ec2-metadata-token: $IMDS_TOKEN" http://169.254.169.254/latest/meta-data/placement/region)
    aws ec2 terminate-instances --instance-ids "$INSTANCE_ID" --region "$REGION"
}
trap terminate_self EXIT

INSTANCE_INDEX=__INSTANCE_INDEX__    # replaced per-instance in launch script
TOTAL_WORKERS=3156
PROJECT_DIR="/home/ubuntu/multi-delogo"   # adjust to actual path on your AMI

mkdir -p "$PROJECT_DIR/logs"
exec > "$PROJECT_DIR/logs/delogo-startup.log" 2>&1

# --- Load non-secret config from .env on the AMI (S3 URLs, worker counts, region) ---
set -a
source "$PROJECT_DIR/.env"
set +a

# --- Launch 4 workers in parallel ---
cd "$PROJECT_DIR"
PIDS=()
for LOCAL_IDX in 0 1 2 3; do
    WORKER_INDEX=$(( INSTANCE_INDEX * 4 + LOCAL_IDX ))
    INPUT_CSV_S3="$INPUT_CSV_S3" \
    OUTPUT_PREFIX="$OUTPUT_PREFIX" \
    OUTPUT_CSV_S3="$OUTPUT_CSV_S3" \
    TOTAL_WORKERS="$TOTAL_WORKERS" \
    WORKER_INDEX="$WORKER_INDEX" \
    python3 pipeline.py > "$PROJECT_DIR/logs/worker_${WORKER_INDEX}.log" 2>&1 &
    PIDS+=($!)
done

# --- Wait for all 4 to finish (max 8 hours) ---
EXIT_CODE=0
DEADLINE=$(( $(date +%s) + 28800 ))
for PID in "${PIDS[@]}"; do
    while kill -0 "$PID" 2>/dev/null; do
        if [ "$(date +%s)" -ge "$DEADLINE" ]; then
            echo "Timeout reached. Killing worker PID $PID"
            kill "$PID" 2>/dev/null
            break
        fi
        sleep 10
    done
    wait "$PID" 2>/dev/null || EXIT_CODE=$?
done

echo "All workers done. Exit code: $EXIT_CODE"
UBUNTU_SCRIPT
