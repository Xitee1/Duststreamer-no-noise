# Duststreamer

The Duststreamer is essentially just gstreamer with a minimal wrapper for the use-case of vacuum robot camera streaming within Valetudo.

The only magic lives within the build pipeline of this repo, as that manages to build a single statically linked self-contained binary using zig because zig is cool and LLMs know how to use it to build static binaries. The people making zig also seem cool. Shoutout to them.
