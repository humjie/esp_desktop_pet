# Tk draws this small window through X11; no OpenGL or GPU toolkit is used.
wm title . "Quote Cards"
wm geometry . 560x380
wm minsize . 380 300
. configure -background #f5f2eb

frame .card -background #f5f2eb -cursor hand2
pack .card -fill both -expand 1 -padx 32 -pady {28 12}
label .card.category -textvariable category -background #f5f2eb \
    -foreground #6b786e -font {TkDefaultFont 10}
pack .card.category -anchor w
label .card.quote -textvariable quote_text -background #f5f2eb \
    -foreground #26372e -font {TkDefaultFont 20} -justify left \
    -anchor w -wraplength 490
pack .card.quote -fill both -expand 1 -pady 16
label .card.hint -text "Click for another card (Space / right arrow also works)" \
    -background #f5f2eb -foreground #6b786e -font {TkDefaultFont 9}
pack .card.hint -anchor w
foreach widget {.card .card.category .card.quote .card.hint} {
    bind $widget <Button-1> nextCard
}
bind .card <Configure> {.card.quote configure -wraplength [expr {max(100, %w)}]}

button .source -text "Open source" -command openSource -relief flat \
    -background #f5f2eb -activebackground #f5f2eb -foreground #6b786e -cursor hand2
pack .source -anchor w -padx 28 -pady {0 20}
bind . <space> nextCard
bind . <Right> nextCard
bind . <Escape> {destroy .}
nextCard
