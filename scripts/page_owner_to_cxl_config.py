#!/usr/bin/env python3
import sys

PAGE_SIZE = 4096
POOL_OWNER = 100


def parse_nodes(arg):
    return [int(x, 0) for x in arg.split(",") if x.strip()]


def merge_pages(pages):
    pages = sorted(set(pages))
    if not pages:
        return []
    ranges = []
    start = prev = pages[0]
    for p in pages[1:]:
        if p == prev + 1:
            prev = p
            continue
        ranges.append((start, prev))
        start = prev = p
    ranges.append((start, prev))
    return ranges


def main():
    if len(sys.argv) < 2:
        print("usage: page_owner_to_cxl_config.py PAGE_OWNER_CI [OUT.csv] [NODES]", file=sys.stderr)
        print("  NODES: comma-separated node_ids (default: 0)", file=sys.stderr)
        sys.exit(1)

    in_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "-"
    nodes = parse_nodes(sys.argv[3]) if len(sys.argv) > 3 else [0]

    pages = []
    with open(in_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            page = int(parts[0], 0)
            owner = int(parts[1], 0)
            if owner == POOL_OWNER:
                pages.append(page)

    ranges = merge_pages(pages)

    out = sys.stdout if out_path == "-" else open(out_path, "w")
    try:
        out.write("# node_id,start,size,type,target\n")
        for start_page, end_page in ranges:
            start_addr = start_page * PAGE_SIZE
            size = (end_page - start_page + 1) * PAGE_SIZE
            for node_id in nodes:
                out.write(f"{node_id},0x{start_addr:x},0x{size:x},pool,{POOL_OWNER}\n")
    finally:
        if out is not sys.stdout:
            out.close()


if __name__ == "__main__":
    main()
