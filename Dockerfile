FROM debian:trixie AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates curl xz-utils git file python3 python3-pip \
        build-essential pkg-config nasm yasm flex bison \
        llvm clang upx-ucl binutils-aarch64-linux-gnu && \
    rm -rf /var/lib/apt/lists/*

RUN pip3 install --break-system-packages 'meson>=1.4.0' ninja

ARG ZIG_VERSION=0.16.0
RUN curl -L "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz" -o /tmp/zig.tar.xz && \
    tar -xf /tmp/zig.tar.xz -C /opt && \
    ln -s "/opt/zig-x86_64-linux-${ZIG_VERSION}" /opt/zig && \
    rm /tmp/zig.tar.xz
ENV PATH="/opt/zig:${PATH}"

RUN printf '#!/bin/sh\nexec zig cc -target aarch64-linux-musl -mcpu=cortex_a53 "$@"\n' > /usr/local/bin/aarch64-cc && \
    printf '#!/bin/sh\nexec zig c++ -target aarch64-linux-musl -mcpu=cortex_a53 "$@"\n' > /usr/local/bin/aarch64-cxx && \
    chmod +x /usr/local/bin/aarch64-cc /usr/local/bin/aarch64-cxx

WORKDIR /root/

ARG GST_REPO_URL=https://gitlab.freedesktop.org/vivia/gstreamer.git
RUN for i in 1 2 3 4 5 6 7 8 9 10; do \
        git clone --depth=1 --branch=dustsrc "$GST_REPO_URL" gstreamer && break \
        || { echo "clone attempt $i failed, retrying..."; rm -rf gstreamer; sleep 5; }; \
    done && test -d gstreamer

# Merely compiling in v4l2 otherwise makes it touch the allwinner kernel inappropriately, causing it to lock up
RUN sed -i '/GST_DEVICE_PROVIDER_REGISTER (v4l2deviceprovider/d' \
    /root/gstreamer/subprojects/gst-plugins-good/sys/v4l2/gstv4l2.c && \
    ! grep -q 'GST_DEVICE_PROVIDER_REGISTER (v4l2deviceprovider' \
    /root/gstreamer/subprojects/gst-plugins-good/sys/v4l2/gstv4l2.c && \
    echo "v4l2deviceprovider registration deleted from gstv4l2.c"

COPY res/aarch64-musl.ini /root/aarch64-musl.ini

WORKDIR /root/gstreamer

RUN for i in 1 2 3 4 5 6 7 8 9 10; do \
        rm -rf build && \
        meson setup build \
            --cross-file /root/aarch64-musl.ini \
            --default-library=static \
            --buildtype=release \
            -Dauto_features=disabled \
            -Dorc=enabled \
            -Dorc-compiler=disabled \
            -Dorc-source=subproject \
            -Dprefix=/root/gst-install \
            -Dgst-full=enabled \
            -Dgst-full-target-type=static_library \
            -Dgst-full-plugins="*" \
            -Dbase=enabled -Dgood=enabled -Dugly=disabled -Dbad=enabled -Dlibav=enabled \
            -Drs=disabled -Dges=disabled -Ddevtools=disabled -Drtsp_server=disabled \
            -Dgst-examples=disabled -Dpython=disabled -Dsharp=disabled -Dgtk=disabled \
            -Dintrospection=disabled -Dtools=disabled -Dgpl=enabled \
            -DFFmpeg:gpl=enabled \
            -DFFmpeg:encoders=enabled \
            -DFFmpeg:mpeg1video_encoder=enabled \
            -DFFmpeg:mjpeg_encoder=enabled \
            -Dgst-plugins-bad:dustsrc=enabled \
            -Dgst-plugins-bad:mpegtsmux=enabled \
            -Dgst-plugins-good:udp=enabled \
            -Dgst-plugins-good:jpeg=enabled \
            -Dgst-plugins-good:videofilter=enabled \
            -Dgst-plugins-good:v4l2=enabled \
            -Dgst-plugins-good:v4l2-probe=false \
            -Dgst-plugins-base:videoconvertscale=enabled \
            -Dgst-plugins-base:videorate=enabled \
        && break \
        || { echo "=== meson setup attempt $i failed, retrying... ==="; sleep 10; }; \
    done && test -f build/build.ninja

RUN meson compile -C build


COPY res/duststreamer.c /root/gstreamer/duststreamer/duststreamer.c
COPY res/launcher.meson /root/gstreamer/duststreamer/meson.build
RUN echo "subdir('duststreamer')" >> /root/gstreamer/meson.build

ARG DUSTSTREAMER_VERSION="unknown"

RUN meson setup build --reconfigure \
        --cross-file /root/aarch64-musl.ini \
        --default-library=static \
        --buildtype=release \
        -Dc_args="-DDUSTSTREAMER_VERSION=\\\"${DUSTSTREAMER_VERSION}\\\"" \
        -Dauto_features=disabled \
        -Dorc=enabled \
        -Dorc-compiler=disabled \
        -Dorc-source=subproject \
        -Dprefix=/root/gst-install \
        -Dgst-full=enabled \
        -Dgst-full-target-type=static_library \
            -Dgst-full-plugins="*" \
            -Dbase=enabled -Dgood=enabled -Dugly=disabled -Dbad=enabled -Dlibav=enabled \
            -Drs=disabled -Dges=disabled -Ddevtools=disabled -Drtsp_server=disabled \
            -Dgst-examples=disabled -Dpython=disabled -Dsharp=disabled -Dgtk=disabled \
            -Dintrospection=disabled -Dtools=disabled -Dgpl=enabled \
            -DFFmpeg:gpl=enabled \
            -DFFmpeg:encoders=enabled \
            -DFFmpeg:mpeg1video_encoder=enabled \
            -DFFmpeg:mjpeg_encoder=enabled \
            -Dgst-plugins-bad:dustsrc=enabled \
            -Dgst-plugins-bad:mpegtsmux=enabled \
            -Dgst-plugins-good:udp=enabled \
            -Dgst-plugins-good:jpeg=enabled \
            -Dgst-plugins-good:videofilter=enabled \
            -Dgst-plugins-good:v4l2=enabled \
            -Dgst-plugins-good:v4l2-probe=false \
            -Dgst-plugins-base:videoconvertscale=enabled \
            -Dgst-plugins-base:videorate=enabled
RUN meson compile -C build duststreamer

RUN meson install -C build

RUN file /root/gstreamer/build/duststreamer/duststreamer
RUN cp /root/gstreamer/build/duststreamer/duststreamer /root/duststreamer
RUN aarch64-linux-gnu-strip /root/duststreamer
RUN upx --lzma --best /root/duststreamer || echo "UPX failed, keeping unstripped binary"
RUN ldd /root/duststreamer 2>&1 | grep -i "not a dynamic executable" || true

FROM scratch
COPY --from=builder /root/duststreamer /duststreamer
CMD ["/duststreamer"]
