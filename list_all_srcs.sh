#!/bin/bash
# Script to list all source files for Makefile.am format and write to listed_srcfiles.txt

output_file="listed_srcfiles.txt"

# Write header
{
    echo "libcscore_la_SOURCES = \\"
    echo "    cscore.h \\"
    echo "    cscore.cc \\"
} > "$output_file"

# List files in inc/ and src/ recursively, sort, and format
find inc/ src/ branch/ btb/ prefetcher/ replacement/ -type f | sort | sed 's/^/    /;s/$/ \\/' >> "$output_file"