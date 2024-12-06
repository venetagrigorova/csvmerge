#!/bin/bash

# Define an array of .c files
files=("base.c" "base.c" "base.c")

# Directory containing CSV files
csv_dir="/localtmp/efficient24/"

# Output text file for all performance statistics
output_txt="combined_perf_output.txt"

# Clear the output file or create it if it doesn't exist
echo "Performance Statistics for Multiple Executables" > "$output_txt"
echo "-------------------------------------------------" >> "$output_txt"

# Loop through each .c file
for file in "${files[@]}"
do
    # Extract the base name without extension
    exe_name="${file%.c}"

    # Compile the .c file
    gcc "$file" -o "$exe_name"
    
    # Check if compilation succeeded
    if [ $? -eq 0 ]; then
        echo "Compiled $file successfully. Running perf stat..."

        # Add a header for each executable's output in the combined text file
        echo -e "\nPerformance stats for $exe_name:" >> "$output_txt"
        
        # Run perf stat and append stderr output to the combined text file
        perf stat ./"$exe_name" "${csv_dir}f1.csv" "${csv_dir}f2.csv" "${csv_dir}f3.csv" "${csv_dir}f4.csv" 1>/dev/null 2>> "$output_txt"
        
        # Add a separator between outputs for better readability
        echo "-------------------------------------------------" >> "$output_txt"
    else
        echo "Failed to compile $file."
    fi
done
