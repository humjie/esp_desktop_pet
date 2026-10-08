CC = cc
CFLAGS = -Os -Wall -Wextra -Wpedantic
PACKAGES = tk json-c

.PHONY: all clean
all: .local/bin/quote-cards

.local/bin/quote-cards: app.c
	mkdir -p .local/bin
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PACKAGES)) app.c -o $@ $$(pkg-config --libs $(PACKAGES))

clean:
	rm -f .local/bin/quote-cards
