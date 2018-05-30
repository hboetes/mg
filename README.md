# mg with wide display support.

To disable, add the following line to your `.mg`

```
set-default-mode "raw"
```

# Upstream support of UTF-8

Please read [Theo de Raadt's
thoughts](https://marc.info/?t=152753252500001&r=1&w=2) on adding
UTF-8 support to mg. Patches to make mg 8-bit clean are quite
welcome!

# PORTING MG AND USING LIBBSD

I've maintained and ported mg for quite some time now and at first it
was easy recently it got harder and harder since it was a moving
target. Especially the inclusion of some system specific libraries since
about 2 years ago made it too much of an effort for my humble coding
skills.

So recently Jasper Lievisse Adriaanse asked me to try it again and I
restarted working on the project and ran into exactly the same problems
again. While googling for solutions I ran into libbsd:

  http://libbsd.freedesktop.org/wiki/

It's a porting library for OpenBSD code! And after installing that it
was a piece of pie to get mg ported again.

## PORTING TO ALL OTHER PLATFORMS

Okay, that was debian. Now I have to get the rest of all the previously
suported platforms working again. All help is welcome and as always:
Please provide patches that do not break stuff for other platforms.

## BUILDING MG

So, basic instructions for building mg:

 - Get the libbsd and libncurses dev packages installed.
 - Run the following commands:

```
make
sudo make install
```

## STATIC BUILDS

I recently figured out how to make really portable static builds: On an
alpine linux system, build with the command:
```
make STATIC=yesplease
```
glibc does not really support static binaries. https://www.musl-libc.org/
does not have this problem.


## USING CVS

This code is the cvs checkout from the OpenBSD project so if you install
cvs you can see what I changed to port mg. Like this:

```
cvs diff -uw
```
