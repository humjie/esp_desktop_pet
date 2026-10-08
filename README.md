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

All entries are stored together in **quotes.json**, which the app reads.
**quotes.md** is the readable Markdown copy of the whole collection.
Add an object to the list in `quotes.json`:

```json
{
  "text": "Write your quote or idea here.",
  "category": "help me think",
  "source": "https://example.com/your-source"
}
```

Separate objects with commas. Use `\n` inside the text for line breaks.
After editing JSON, update the Markdown copy:

```sh
python3 app.py --export-md
```

Restart the app to load additions or edits. Keep entries short so they fit the card.
Edit `quotes.json`; regenerating Markdown replaces `quotes.md`.

The two original entries are included in both files.
