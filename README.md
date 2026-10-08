# Quote Cards moved to Desk Pet

The collection now lives in [deskpet/quotes](../deskpet/quotes/), and the card UI
runs on the ESP32-S3-BOX-3. The desktop application and its login autostart entry
have been removed.

Tap the pet screen's top-left quarter for a random quote and its category, tap
there again for a different quote, and tap elsewhere to return to the pet.

Edit `../deskpet/quotes/quotes.json`, connect the pet by USB, then run from this
`quote-cards` directory:

```bash
../deskpet/update-quotes.sh
```

The script updates the readable Markdown copy, builds and flashes firmware, and
restores the host service afterward. See [Desk Pet's README](../deskpet/README.md)
for quote-update options and setup instructions.
