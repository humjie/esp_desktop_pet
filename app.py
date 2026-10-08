#!/usr/bin/env python3
"""A small desktop flash card for the Markdown + JSON quote collection."""

import json
from pathlib import Path
import random
import tkinter as tk
from tkinter import messagebox
import webbrowser


QUOTES_DIR = Path(__file__).resolve().parent / "quotes"
BACKGROUND = "#f5f2eb"
FOREGROUND = "#26372e"
MUTED = "#6b786e"


def load_quotes(directory=QUOTES_DIR):
    quotes = []
    for path in sorted(directory.glob("*.md")):
        metadata = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
        text = path.read_text(encoding="utf-8").strip()
        if not text or not isinstance(metadata, dict) or not all(
            isinstance(metadata.get(key), str) and metadata[key].strip()
            for key in ("category", "source")
        ):
            raise ValueError(f"{path.stem}: quote, category and source must be nonempty.")
        quotes.append({"text": text, "category": metadata["category"],
                       "source": metadata["source"]})
    if not quotes:
        raise ValueError(f"No quotes found in {directory}.")
    return quotes


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
    root = tk.Tk()
    root.withdraw()
    try:
        quotes = load_quotes()
    except (OSError, ValueError) as error:
        messagebox.showerror("Could not load quotes", str(error), parent=root)
        root.destroy()
        return
    FlashCards(root, quotes)
    root.deiconify()
    root.mainloop()


if __name__ == "__main__":
    main()
