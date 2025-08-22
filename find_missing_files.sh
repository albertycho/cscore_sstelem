#!/bin/bash

# Directories
orig_base="/nethome/acho44/DIST_CXL/ChampSim"
new_base="/nethome/acho44/SST_TEST/sst-elements/src/sst/elements/cscore"

# Function to compare a subdir (src/inc)
check_missing() {
    subdir=$1
    echo "Checking $subdir..."
    diff <(cd "$orig_base/$subdir" && find . -type f | sort) \
         <(cd "$new_base/$subdir" 2>/dev/null && find . -type f | sort) \
         | grep '^<' | sed 's/^< //'
}

echo "Files missing in cscore but present in ChampSim:"
check_missing src
check_missing inc

