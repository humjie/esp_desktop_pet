#!/usr/bin/env python3
"""A small desktop flash card for the Markdown + JSON quote collection."""

import argparse
import json
from pathlib import Path
import random
import tkinter as tk
import webbrowser


QUOTES_FILE = Path(__file__).resolve().parent / "quotes.json"
BACKGROUND = "#f5f2eb"
FOREGROUND = "#26372e"
MUTED = "#6b786e"


def load_quotes(path=QUOTES_FILE):
    quotes = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(quotes, list) or not quotes:
        raise ValueError(f"{path.name} must contain a nonempty list of quotes.")
    for index, quote in enumerate(quotes, start=1):
        if not isinstance(quote, dict) or not all(
            isinstance(quote.get(key), str) and quote[key].strip()
            for key in ("text", "category", "source")
        ):
            raise ValueError(f"Quote {index}: text, category and source must be nonempty.")
    return quotes


def export_markdown(quotes):
    sections = ["# Quotes\n"]
    for index, quote in enumerate(quotes, start=1):
        sections.append(
            f"## {index}\n\n{quote['text']}\n\n"
            f"**Category:** {quote['category']}\n\n"
            f"**Source:** {quote['source']}\n"
        )
    QUOTES_FILE.with_suffix(".md").write_text("\n".join(sections), encoding="utf-8")


class FlashCards:
    def __init__(self, root, quotes):
        self.quotes = quotes
        self.current = None
        root.title("Quote Cards")
        root.geometry("560x380")
        root.minsize(380, 300)
        root.configure(bg=BACKGROUND)

        card = tk.Frame(root, bg=BACKGROUND, cursor="hand2")
        card.pack(fill="both", expand=True, padx=32, pady=(28, 12))
        self.category = tk.Label(
            card, bg=BACKGROUND, fg=MUTED, font=("TkDefaultFont", 10)
        )
        self.category.pack(anchor="w")
        self.quote = tk.Label(
            card, bg=BACKGROUND, fg=FOREGROUND,
            font=("TkDefaultFont", 20), justify="left", anchor="w",
            wraplength=490,
        )
        self.quote.pack(fill="both", expand=True, pady=16)
        hint = tk.Label(
            card, text="Click for another card · Space / → also works",
            bg=BACKGROUND, fg=MUTED, font=("TkDefaultFont", 9),
        )
        hint.pack(anchor="w")
        for widget in (card, self.category, self.quote, hint):
            widget.bind("<Button-1>", self.next_card)
        card.bind("<Configure>", lambda event: self.quote.configure(
            wraplength=max(100, event.width)
        ))

        tk.Button(
            root, text="Open source ↗", command=self.open_source,
            relief="flat", bg=BACKGROUND, fg=MUTED, cursor="hand2",
            activebackground=BACKGROUND,
        ).pack(anchor="w", padx=28, pady=(0, 20))
        root.bind("<space>", self.next_card)
        root.bind("<Right>", self.next_card)
        root.bind("<Escape>", lambda event: root.destroy())
        self.next_card()

    def next_card(self, event=None):
        choices = [quote for quote in self.quotes if quote is not self.current]
        self.current = random.choice(choices or self.quotes)
        self.category.configure(text=self.current["category"])
        self.quote.configure(text=self.current["text"])

    def open_source(self):
        webbrowser.open(self.current["source"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-md", action="store_true",
                        help="Update quotes.md from quotes.json without opening the app")
    args = parser.parse_args()
    try:
        quotes = load_quotes()
    except (OSError, ValueError) as error:
        raise SystemExit(f"Could not load quotes: {error}") from error
    if args.export_md:
        export_markdown(quotes)
        return
    root = tk.Tk()
    FlashCards(root, quotes)
    root.mainloop()


if __name__ == "__main__":
    main()
