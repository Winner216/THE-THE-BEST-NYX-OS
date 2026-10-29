#!/usr/bin/env python3
"""
extract-pdf-text.py — Extract readable text from PDFs (host-side).

Uses pdftotext (from poppler-utils) to reliably extract text from PDFs,
handling FlateDecode compression, UTF-16BE encoding, custom font
encodings, and other real-world PDF complexity that the Nyx OS kernel's
simple parser cannot handle.

Stores the extracted text as a companion .txt file that the Nyx OS PDF
viewer can read directly.

Usage:
    python3 tools/extract-pdf-text.py path/to/file.pdf

Output: path/to/file.txt (alongside the PDF)
"""

import sys, os, subprocess, re


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__)
        sys.exit(0 if len(sys.argv) < 2 else 1)

    pdf_path = sys.argv[1]

    if not os.path.exists(pdf_path):
        print(f"  ✗  File not found: {pdf_path}")
        sys.exit(1)

    if pdf_path[:4].lower() != '.pdf' and not pdf_path.lower().endswith('.pdf'):
        print(f"  ⚠  '{pdf_path}' doesn't look like a PDF file")

    # Verify it's a PDF
    with open(pdf_path, 'rb') as f:
        magic = f.read(4)
    if magic != b'%PDF':
        print(f"  ✗  '{pdf_path}' doesn't start with %PDF — not a valid PDF")
        sys.exit(1)

    # Use pdftotext to extract text
    try:
        result = subprocess.run(
            ['pdftotext', pdf_path, '-'],
            capture_output=True, timeout=30
        )
    except FileNotFoundError:
        print("  ✗  pdftotext not found. Install poppler-utils:")
        print("     sudo apt install poppler-utils")
        sys.exit(1)
    except subprocess.TimeoutExpired:
        print("  ✗  pdftotext timed out")
        sys.exit(1)

    if result.returncode != 0:
        print(f"  ✗  pdftotext failed (exit code {result.returncode})")
        if result.stderr:
            print(f"     {result.stderr.decode(errors='replace').strip()}")
        sys.exit(1)

    text = result.stdout.decode('utf-8', errors='replace')

    # Count pages by looking for form feed characters or check with pdfinfo
    page_count = 1
    try:
        info_result = subprocess.run(
            ['pdfinfo', pdf_path],
            capture_output=True, timeout=15
        )
        if info_result.returncode == 0:
            info_text = info_result.stdout.decode('utf-8', errors='replace')
            pm = re.search(r'Pages:\s+(\d+)', info_text)
            if pm:
                page_count = int(pm.group(1))
    except (FileNotFoundError, subprocess.TimeoutExpired):
        # Fallback: count form feeds in text
        ff_count = text.count('\f')
        if ff_count > 0:
            page_count = ff_count + 1

    if not text.strip():
        print(f"  ⚠  No text extracted from '{os.path.basename(pdf_path)}'")
        print(f"     (The PDF may be scanned images with no text layer)")
        sys.exit(0)  # Not an error — just no extractable text

    # Write companion .txt file
    txt_path = pdf_path[:pdf_path.rfind('.')] + '.txt'

    header = f"[Extracted from {os.path.basename(pdf_path)} - {page_count} page(s)]\n\n"
    full = header + text

    with open(txt_path, 'w', encoding='utf-8', errors='replace') as f:
        f.write(full)

    print(f"  ✔  Extracted {len(full)} chars from {page_count} page(s)")
    print(f"     → {txt_path}")

    # Preview
    preview = text[:120].replace('\n', '↵')
    print(f"     Preview: {preview}...")


if __name__ == '__main__':
    main()
