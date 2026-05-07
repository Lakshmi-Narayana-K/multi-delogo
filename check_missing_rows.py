#!/usr/bin/env python3
"""Print row_index values in 0..551 that are absent from final_output.csv."""
import csv

PATH = "final_output.csv"
LOW, HIGH = 0, 551

present = set()
with open(PATH, newline="") as f:
    for row in csv.DictReader(f):
        present.add(int(row["row_index"]))

expected = set(range(LOW, HIGH + 1))
missing = sorted(expected - present)

print(f"Rows present in [{LOW}, {HIGH}]: {len(present & expected)}")
print(f"Missing ({len(missing)}): {missing}")