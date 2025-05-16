Mac OS X Build Instructions and Notes
====================================
This guide will show you how to build smileycoind(headless client) for OSX.

Notes
-----

* Tested on OS X 10.6 through 10.9 on 64-bit Intel processors only.
Older OSX releases or 32-bit processors are no longer supported.

* All of the commands should be executed in a Terminal application. The
built-in one is located in `/Applications/Utilities`.

Preparation
-----------

You need to install XCode with all the options checked so that the compiler
and everything is available in /usr not just /Developer. XCode should be
available on your OS X installation media, but if not, you can get the
current version from https://developer.apple.com/xcode/. If you install
Xcode 4.3 or later, you'll need to install its command line tools. This can
be done in `Xcode > Preferences > Downloads > Components` and generally must
be re-done or updated every time Xcode is updated.

There's an assumption that you already have `git` installed, as well. If
not, it's the path of least resistance to install [Github for Mac](https://mac.github.com/)
(OS X 10.7+) or
[Git for OS X](https://code.google.com/p/git-osx-installer/). It is also
available via Homebrew or MacPorts.

You will also need to install [Homebrew](http://brew.sh)
or [MacPorts](https://www.macports.org/) in order to install library
dependencies. It is recommended to use MacPorts, it is a little easier because you can just install the
dependencies immediately - no other work required.

The installation of the actual dependencies is covered in the Instructions
sections below.

Instructions: MacPorts
----------------------

### Install dependencies

Installing the dependencies using MacPorts is very straightforward.

    sudo port install boost db53@+no_java openssl miniupnpc autoconf pkgconfig automake libtool

Optional: install Qt4

    sudo port install qt4-mac qrencode protobuf-cpp

### Building `smileycoind`

1. Clone the github tree to get the source code and go into the directory.

        git clone https://github.com/tutor-web/smileyCoin.git
        cd smileyCoin

2.  Build smileycoind (and Smileycoin-Qt, if configured):

        ./autogen.sh
        ./configure
        make

3.  It is a good idea to build and run the unit tests, too:

        make check

Instructions: Homebrew
----------------------

#### Install dependencies using Homebrew

        brew install autoconf automake berkeley-db boost miniupnpc openssl pkg-config protobuf qt



### Building `smileycoind`

1. Clone the github tree to get the source code and go into the directory.

        git clone https://github.com/tutor-web/smileyCoin.git
        cd smileyCoin

2.  Build smileycoind:

        ./autogen.sh
        ./configure --with-incompatible-bdb
        make

3.  It is a good idea to build and run the unit tests, too:

        make check

Creating a release build
------------------------
You can ignore this section if you are building `smileycoind` for your own use.

smileycoind/smileycoin-cli binaries are not included in the Smileycoin-Qt.app bundle.

If you are building `smileycoind` or `Smileycoin-Qt` for others, your build machine should be set up
as follows for maximum compatibility:

We haven't found a way to make the Qt compatible with older osx than the current... these following flags probably won't do anything.

All dependencies should be compiled with these flags:

 -mmacosx-version-min=10.6
 -arch x86_64
 -isysroot $(xcode-select --print-path)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk

For MacPorts, that means editing your macports.conf and setting
`macosx_deployment_target` and `build_arch`:

    macosx_deployment_target=10.6
    build_arch=x86_64

... and then uninstalling and re-installing, or simply rebuilding, all ports.

Download and install this: qt-opensource-mac-x64-clang-5.4.0.dmg
Run the following:

    cd ~/Qt5.4.0/5.4/clang_64/include
    for MODULE in $(find ../lib -type d -name Headers); do ln -s $MODULE $(echo $MODULE | cut -d"/" -f3 | cut -d"." -f1) ; done
    export CXXFLAGS=-std=c++11

Then navigate back to smileyCoin/ and run the followoing:

    PKG_CONFIG_PATH=~/Qt5.4.0/5.4/clang_64/lib/pkgconfig ac_cv_path_MOC=~/Qt5.4.0/5.4/clang_64/bin/moc ac_cv_path_UIC=~/Qt5.4.0/5.4/clang_64/bin/uic ac_cv_path_RCC=~/Qt5.4.0/5.4/clang_64/bin/rcc ac_cv_path_LRELEASE=~/Qt5.4.0/5.4/clang_64/bin/lrelease ./configure --with-gui=qt5
    make
    sudo easy_install argparse
    make deploy

As of December 2012, the `boost` port does not obey `macosx_deployment_target`.
Download `http://gavinandresen-bitcoin.s3.amazonaws.com/boost_macports_fix.zip`
for a fix.

Once dependencies are compiled, see release-process.md for how the Smileycoin-Qt.app
bundle is packaged and signed to create the .dmg disk image that is distributed.

Running
-------

It's now available at `./smileycoind`, provided that you are still in the `src`
directory. We have to first create the RPC configuration file, though.

Run `./smileycoind` to get the filename where it should be put, or just try these
commands:

    echo -e "rpcuser=smileycoinrpc\nrpcpassword=$(xxd -l 16 -p /dev/urandom)" > "/Users/${USER}/Library/Application Support/Smileycoin/smileycoin.conf"
    chmod 600 "/Users/${USER}/Library/Application Support/Smileycoin/smileycoin.conf"

When next you run it, it will start downloading the blockchain, but it won't
output anything while it's doing this. This process may take several hours;
you can monitor its process by looking at the debug.log file, like this:

    tail -f $HOME/Library/Application\ Support/Smileycoin/debug.log

Other commands:

    ./smileycoind -daemon # to start the smileycoin daemon.
    ./smileycoin-cli --help  # for a list of command-line options.
    ./smileycoin-cli help    # When the daemon is running, to get a list of RPC commands
