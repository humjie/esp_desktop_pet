# Quote Cards

A small collection of quotes, ideas and learnings, with a desktop flash card.
Built with C, Tk and json-c on Linux. The native executable lives in the private
`.local/bin/` folder in this repo. No Python, virtual environment, package downloads,
background server or global installation is needed. This private installation
uses the system Tk and json-c libraries; a Python virtual environment is unnecessary.

## Run

```sh
./setup.sh
./run.sh
```

Setup compiles the app and enables startup at desktop login after reboot.
Run `run.sh` by its full path from anywhere. Rerun setup after moving the repo.
Only one instance runs at a time. Closing it keeps it closed until you launch it
again or log in again.

Build requirements: a C compiler, make, pkg-config, Tk and json-c development
headers. These are already installed on this computer. On another Ubuntu/Debian
machine: `sudo apt install build-essential pkg-config tk-dev libjson-c-dev`.
Running requires the matching Tk and json-c libraries, X11 (or XWayland),
`flock`, and `xdg-open` for source links.

The app waits for input without polling or animation. Tk draws via X11 without
OpenGL, Vulkan or a GPU rendering library; the launcher also forces software
rendering for OpenGL libraries. The desktop compositor may use GPU resources
independently. Opening a source launches your normal browser, which has its own
resource and GPU settings.

To disable startup, remove `~/.config/autostart/quote-cards.desktop` (or the
matching file under `$XDG_CONFIG_HOME/autostart` if configured).

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
./run.sh --export-md
```

Restart the app to load additions or edits. Keep entries short so they fit the card.
Edit `quotes.json`; regenerating Markdown replaces `quotes.md`.

The two original entries are included in both files.
