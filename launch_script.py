import boto3
import csv
import time
from datetime import datetime

# --- Config ---
AMI_ID            = "ami-0ab723019cf22733e"        # from Step 1
INSTANCE_TYPE     = "c6a.2xlarge"
REGION            = "ap-south-1"
# Instance profile name (not the role name) — must grant S3 read/write for your
# buckets/keys, ec2:TerminateInstances for self-terminate in user_data, and ssm if used.
IAM_INSTANCE_PROFILE = "ib-logo-replacement"
SUBNET_ID         = "subnet-02f9087a77db67cb8"     # your subnet
SECURITY_GROUP_ID = "sg-029a09c733bba1486"         # your SG (needs no inbound, outbound to S3/internet)
TOTAL_INSTANCES   = 3
BATCH_SIZE        = 1                     # smaller batches to avoid throttling
SLEEP_BETWEEN_BATCHES = 30                 # seconds to wait between batches
LAUNCH_LOG_CSV    = "launch_log.csv"

ec2 = boto3.client("ec2", region_name=REGION)

with open("user_data_template.sh") as f:
    user_data_template = f.read()

launched_ids = []
failed_indices = []

LOG_FIELDNAMES = ["instance_index", "instance_id", "status", "launched_at", "error"]

with open(LAUNCH_LOG_CSV, "w", newline="") as f:
    csv.DictWriter(f, fieldnames=LOG_FIELDNAMES).writeheader()


def log_row(row: dict):
    with open(LAUNCH_LOG_CSV, "a", newline="") as f:
        csv.DictWriter(f, fieldnames=LOG_FIELDNAMES).writerow(row)


def launch_batch(start, end):
    print(f"[config] user_data loaded: {len(user_data_template)} bytes")
    print(f"[config] first line: {user_data_template.splitlines()[0]}")
    has_placeholder = "__INSTANCE_INDEX__" in user_data_template
    print(f"[config] has __INSTANCE_INDEX__ placeholder: {has_placeholder}")
    for instance_index in range(start, end):
        user_data = user_data_template.replace("__INSTANCE_INDEX__", str(instance_index))

        try:
            response = ec2.run_instances(
                ImageId=AMI_ID,
                InstanceType=INSTANCE_TYPE,
                MinCount=1,
                MaxCount=1,
                UserData=user_data,
                IamInstanceProfile={"Name": IAM_INSTANCE_PROFILE},
                NetworkInterfaces=[{
                    "DeviceIndex": 0,
                    "SubnetId": SUBNET_ID,
                    "Groups": [SECURITY_GROUP_ID],
                    "AssociatePublicIpAddress": True,
                }],
                TagSpecifications=[{
                    "ResourceType": "instance",
                    "Tags": [
                        {"Key": "Name",         "Value": f"ibhubs-logo-worker-{instance_index}"},
                        {"Key": "project_name", "Value": "ibhubs-logo-replacement"},
                    ]
                }],
            )
            instance_id = response["Instances"][0]["InstanceId"]
            launched_ids.append(instance_id)
            print(f"[{instance_index}] Launched {instance_id}")
            log_row({
                "instance_index": instance_index,
                "instance_id":    instance_id,
                "status":         "launched",
                "launched_at":    datetime.now().isoformat(),
                "error":          "",
            })
        except Exception as e:
            failed_indices.append(instance_index)
            print(f"[{instance_index}] FAILED to launch: {e}")
            log_row({
                "instance_index": instance_index,
                "instance_id":    "",
                "status":         "failed",
                "launched_at":    datetime.utcnow().isoformat(),
                "error":          str(e),
            })

    print(f"Batch {start}-{end-1} done")


# Launch in batches
for batch_start in range(0, TOTAL_INSTANCES, BATCH_SIZE):
    batch_end = min(batch_start + BATCH_SIZE, TOTAL_INSTANCES)
    launch_batch(batch_start, batch_end)
    if batch_end < TOTAL_INSTANCES:
        print(f"Sleeping {SLEEP_BETWEEN_BATCHES}s before next batch...")
        time.sleep(SLEEP_BETWEEN_BATCHES)

print(f"\nDone. Launched: {len(launched_ids)}  Failed: {len(failed_indices)}")
if failed_indices:
    print(f"Failed instance indices: {failed_indices}")
print(f"Launch log saved to: {LAUNCH_LOG_CSV}")