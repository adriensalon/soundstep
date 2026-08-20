# soundstep

Modern music player with P2P library sharing for Windows, Linux and Android over LAN and IPv6, connected devices share their local library to their connected peers.

![](/image/readme_000.png "")

LAN devices are discovered automatically, IPv6 connexions require sharing a code between devices. Shared tracks can be streamed and saved to be listened locally. Built it to listen to my music on my phone when afk.

# Requirements

Building requires CMake > 3.20 and a C++17 compiler. Android crossplatform build requires NDK, Java and Ninja.

# Limitations
 
First version is very rough : still questionnable UX choices, font sizes, some Android screen size bugs, and lack of playlist support. Maybe would be nice to implement metadata fixer from opensource databases, then bitorrent client ?
Release builds are provided for Windows, Linux and Android. If you want to try it out please report bugs with Github issues. 