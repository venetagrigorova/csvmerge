import csv
from collections import defaultdict

def read_csv(file_path):
    with open(file_path, 'r') as f:
        reader = csv.reader(f)
        return [row for row in reader]

# File Relationships:
# - file1 joins with file2 and file3 using their first fields.
# - The second field of file3 joins with the first field of file4.

# Output Format:
# [First field of file4, First field of file1, Second field of file1, 
#  Second field of file2, Second field of file4]

def myjoin(file1, file2, file3, file4):
    # Load CSV files into lists
    file1_data = read_csv(file1)
    file2_data = read_csv(file2)
    file3_data = read_csv(file3)
    file4_data = read_csv(file4)

    # Create dictionaries with lists for multiple matches
    file2_dict = defaultdict(list)
    for key, value in file2_data:
        file2_dict[key].append(value)

    file3_dict = defaultdict(list)
    for key, value in file3_data:
        file3_dict[key].append(value)

    file4_dict = defaultdict(list)
    for key, value in file4_data:
        file4_dict[key].append(value)

    # Iterate through file1 rows
    for file1_key, file1_value in file1_data:
        if file1_key in file2_dict and file1_key in file3_dict:
            for file2_value in file2_dict[file1_key]:
                for file3_value in file3_dict[file1_key]:
                    if file3_value in file4_dict:
                        for file4_value in file4_dict[file3_value]:
                            # Print the output row in the correct order
                            print(",".join([file3_value, file1_key, file1_value, file2_value, file4_value]))

myjoin("./data/a.csv", "./data/b.csv", "./data/c.csv", "./data/d.csv")