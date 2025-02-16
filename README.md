# PORTING MG AND USING LIBBSD

I've maintained and ported mg for quite some time now and at first it was easy
recently it got harder and harder, since it was a moving target. Especially the
inclusion of some system specific libraries since about 2 years ago made it too
much of an effort for my humble coding skills.

So recently Jasper Lievisse Adriaanse asked me to try it again and I restarted
working on the project and ran into exactly the same problems again. While
googling for solutions, I ran into libbsd:

  http://libbsd.freedesktop.org/wiki/

It's a porting library for OpenBSD code! And after installing that, it was a
piece of pie to get mg ported again.

## PORTING TO ALL OTHER PLATFORMS

Okay, that was Linux. Now I have to get the rest of all the previously supported
platforms working again. All help is welcome and as always:  Please provide
patches, that do not break stuff for other platforms.

## BUILDING MG

So, basic instructions for building mg:

 - Install the `libbsd` and `libncurses` dev packages.
 - Choose one of the following methods:

### USING MAKE

```bash
make
sudo make install
```

### USING CMAKE

```bash
mkdir build
cd build
cmake ..
make
sudo make install
```

*Kudos to Leonid Bobrov(@mazocomp) for adding CMAKE support.*

### USING MESON

```bash
meson setup build
meson compile -C build
sudo meson install -C build
```


## STATIC BUILDS (on Linux)

I recently figured out how to make really portable static builds: On an alpine
linux system, build with the command:

```bash
make STATIC=yesplease
```

The default glibc provided with almost any other linux version does not really
support static binaries. https://www.musl-libc.org/ does not have this problem.

To make building static binaries more easy, check the mg-static directory, there
is a script which can build static binaries with support of podman and buildah.


## THE ORIGINAL CODE FROM CVS

This code is the cvs checkout from the OpenBSD project, so if you install cvs
you can see the changes I made to make mg portable; Like this:

```bash
CVS_RSH=ssh cvs diff -uw
```

### OUT OF SYNC

If you noticed portable mg is not in sync with the upstream release,
for example by running `CVS_RSH=ssh cvs -q up -PAd`, feel free to open
an issue, and I'll update it ASAP.


## FEATURE REQUESTS

I just maintain the port, all I do is importing all changes from the upstream
OpenBSD mg repository:

  https://cvsweb.openbsd.org/src/usr.bin/mg/?sortby=date#dirlist

So your best course of action is to send them a feature request, or even better,
send them a patch.
