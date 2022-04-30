FROM ubuntu:jammy
RUN : \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install \
        -y --no-install-recommends \
        build-essential:amd64 \
        curl \
        debhelper \
        desktop-file-utils \
        devscripts \
        dh-exec \
        docbook-to-man \
        docbook-xsl \
        equivs \
        gnome-common \
        gnome-pkg-tools \
        gnome-shell \
        gnupg \
        gsettings-desktop-schemas-dev \
        intltool \
        libdconf-dev \
        libglib2.0-dev \
        libgtk-3-dev \
        libnautilus-extension-dev \
        libsm-dev \
        libvte-2.91-dev \
        libx11-dev \
        meson \
        uuid-dev \
        yelp-tools \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
