#!/usr/bin/env python3
"""Validate quote data and generate Markdown or a flash-resident C table."""

import argparse
import json
from pathlib import Path
import os
import tempfile


def load_quotes(path):
    entries = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or not entries:
        raise ValueError("expected a nonempty JSON array")
    for number, entry in enumerate(entries, 1):
        if not isinstance(entry, dict):
            raise ValueError(f"entry {number} must be an object")
        for field in ("text", "category"):
            value = entry.get(field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"entry {number}: {field} must be a nonempty string")
            if "\0" in value:
                raise ValueError(f"entry {number}: {field} cannot contain a null character")
            value.encode("utf-8")  # Reject isolated surrogate characters.
        if "source" in entry and not isinstance(entry["source"], str):
            raise ValueError(f"entry {number}: source must be a string if supplied")
    return entries


def write_output(path, text):
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=".quotes-", delete=False) as output:
            temporary = output.name
            output.write(text)
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if temporary and os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--c-output", type=Path)
    args = parser.parse_args()
    try:
        entries = load_quotes(args.input)
        if args.markdown:
            sections = ["# Quotes\n"]
            for number, entry in enumerate(entries, 1):
                section = f"## {number}\n\n{entry['text']}\n\n**Category:** {entry['category']}\n"
                if entry.get("source"):
                    section += f"\n**Source:** {entry['source']}\n"
                sections.append(section)
            write_output(args.markdown, "\n".join(sections))
        if args.c_output:
            # Fixed-width octal escapes safely encode UTF-8 and control bytes
            # without universal-character or trigraph ambiguities in C.
            def literal(value):
                escapes = {34: '\\"', 92: "\\\\", 63: "\\?", 10: "\\n", 13: "\\r", 9: "\\t"}
                return '"' + "".join(escapes.get(byte, chr(byte) if 32 <= byte < 127
                                               else f"\\{byte:03o}")
                                    for byte in value.encode("utf-8")) + '"'
            rows = [f"    {{{literal(entry['text'])}, {literal(entry['category'])}}},"
                    for entry in entries]
            write_output(args.c_output, '#include "quotes.h"\n\n'
                         'const quote_entry_t deskpet_quotes[] = {\n' + "\n".join(rows)
                         + '\n};\nconst size_t deskpet_quote_count = '
                         'sizeof(deskpet_quotes) / sizeof(deskpet_quotes[0]);\n')
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, f"Error in quote collection: {error}\n")
    print(f"Validated {len(entries)} quotes.")


if __name__ == "__main__":
    main()
