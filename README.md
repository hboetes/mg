# mg with wide display support.

I just received this amazing patch from S. Giles. For those impatient
people: check it out (tihihi) and add this line to your `.mg`

```
set-default-mode "wide"
```

## Introduction by S. Giles

Hi,

I've got a patch that allows mg to display wide characters, if you're
interested.

It can be turned on by show-wide-mode (better name welcome), and is
fairly limited in regard to what types of wide characters are
displayed. Everything goes through mbrtowc(3), so you get exactly one
supported encoding: whatever LC_* says. Everything else is displayed
as octal escape sequences (as normal current behavior). Motion is
still on a byte level, so multibyte characters are slow to travel
through, and you can insert bytes in the middle of them (which works
fine). A limited version of insert-char is also included, which works
through wchar_t, so that on any system with __STDC_ISO_10646__ set,
inserting unicode codepoints by number is possible.

It also fixes some odd bugs related to wide character display and
extended lines. For example: in a file with enough wide characters
(such as ＡＢＣ) to make a line extend far (say, 200 characters on an
80-wide display), moving to the right one character at a time will (in
20160118) corrupt the display, then eventually segfault, because
vtpute doesn't perform the same octal expansion as vtputc and the
columns get out of sync. This patch makes display.c aware of the
possibility that the bytes and glyphs of the buffers aren't 1:1, so
protects against that.

That said, wide character support complicates a lot of already
complicated logic (for example, vtputs) and relies on wchar_t for
almost everything, adding some unescapable overhead.

If you want to take this patch, please do so. If you think it's too
ugly or not useful, that's also fine.  Let me know if you want me to
rewrite parts of it (or if you see any bugs) or if there are style
conventions I didn't follow. It applies cleanly with patch -i, please
forgive the git-isms.

(And, of course, many thanks for your work in maintaining the port.)

S. Gilles
