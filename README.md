# Quote Cards

A small collection of quotes, ideas and learnings, with a desktop flash card.
Built with Python and Tkinter. No pip packages or build step.

## Run

```sh
python3 app.py
```

Run that command from this folder, or run `app.py` by its full path from anywhere.
Python 3 with Tkinter and a graphical desktop are required. On Ubuntu/Debian,
install Tkinter with `sudo apt install python3-tk` if needed.

Click the card to show a random different entry. With two entries, they alternate.
The small label shows the category. **Open source** opens the source in your
browser. Space and the right arrow change cards; Escape closes the window.

## Add a quote, idea or learning

Create two files in `quotes/` with the same name:

**my-idea.md** — the text shown on the card:

```md
Write your quote or idea here.
```

**my-idea.json** — its category and source:

```json
{
  "category": "help me think",
  "source": "https://example.com/your-source"
}
```

Restart the app to load additions or edits. Markdown files are displayed as plain
text, so keep them short; line breaks and simple numbered lists work well.
The text lives only in Markdown, and the metadata lives only in JSON.

The two original entries are included in `quotes/`.
