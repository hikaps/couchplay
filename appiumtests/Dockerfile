# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Test image for CouchPlay e2e (appiumtests). Bakes the entire environment so
# there is no setup/teardown script: nested KDE/KWin + gamescope + PipeWire +
# virtual input + the mock D-Bus helper + player2/player3 users + per-user audio
# config + the built app + the KDE selenium driver.
#
# Build (from repo root):  podman build -f appiumtests/Dockerfile -t couchplay-e2e .
#
# Run ROOTLESS via distrobox (shares host graphics/input/audio, runs as the host
# user). The entrypoint brings up a user-owned "system" bus + mock helper +
# PipeWire, then runs the suite under an isolated dbus-run-session (nested kwin):
#   distrobox create --image localhost/couchplay-e2e --name cp-test --yes
#   distrobox enter cp-test -- /entrypoint.sh appiumtests/   # full suite
#   distrobox rm cp-test -f                                  # teardown
#
# A bare `podman run` is NOT the supported flow: the image has no USER directive,
# so it would run the rootless entrypoint as root (kwin/pipewire as root is
# untested). Use distrobox, or `podman run --user` with device/audio passthrough.
FROM fedora:43

# --- runtime + build deps ---
RUN dnf install -y --setopt=install_weak_deps=False \
      plasma-workspace kwin-wayland xorg-x11-server-Xwayland \
      gamescope mesa-dri-drivers \
      pipewire pipewire-pulse wireplumber pulseaudio-utils \
      evemu libevdev \
      ruby rubygems \
      python3-flask python3-gobject python3-lxml python3-numpy python3-pyatspi \
      python3-dbus python3-pip python3-virtualenv at-spi2-core gtk3-devel \
      cmake extra-cmake-modules ninja-build gcc-c++ pkg-config git \
      qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qt5compat-devel \
      qt6-qtwayland-devel qt6-qtbase-private-devel \
      kf6-kirigami-devel kf6-ki18n-devel kf6-kcoreaddons-devel kf6-kconfig-devel \
      kf6-kiconthemes-devel kf6-qqc2-desktop-style kf6-kglobalaccel-devel \
      kf6-kwindowsystem-devel kwayland-devel kpipewire-devel \
      plasma-wayland-protocols-devel pipewire-devel polkit-devel \
      polkit-qt6-1-devel dbus-daemon \
 && dnf clean all

# --- app source + build couchplay ---
WORKDIR /src/couchplay
COPY . .
RUN cmake -B build -DBUILD_TESTING=ON \
 && cmake --build build -j"$(nproc)" -t couchplay

# --- KDE selenium driver (source-only; not on PyPI/in distros) ---
# Pinned to a specific commit for reproducibility (it drives the whole suite;
# tracking HEAD would silently change e2e behavior). Bump deliberately.
ARG SWA_REF=6e64f164819284007a1aae9e0f9dc31da91d5f9d
RUN git init /tmp/swa \
 && git -C /tmp/swa remote add origin https://invent.kde.org/sdk/selenium-webdriver-at-spi.git \
 && git -C /tmp/swa fetch --depth 1 origin "$SWA_REF" \
 && git -C /tmp/swa checkout FETCH_HEAD \
 && cmake -B /tmp/swa/build -S /tmp/swa \
 && cmake --build /tmp/swa/build -j"$(nproc)" \
 && cmake --install /tmp/swa/build \
 && rm -rf /tmp/swa

# Pre-install the runner's python deps into SYSTEM python. At runtime the runner
# runs `pip3 install -r requirements.txt` with system pip; rootless it cannot
# write /usr/local, so pre-satisfy everything here (build runs as root). The
# venv below uses --system-site-packages, so it inherits these too.
RUN pip3 install --no-cache-dir -r /usr/share/selenium-webdriver-at-spi/requirements.txt

# --- python test deps (venv inherits system gi/pyatspi/flask/lxml/numpy/dbus) ---
RUN python3 -m venv --system-site-packages /opt/e2e-venv \
 && /opt/e2e-venv/bin/pip install --no-cache-dir Appium-Python-Client pytest

# --- baked environment: test users + per-user audio config ---
RUN groupadd -f couchplay \
 && for u in player2 player3; do \
        useradd -m -G couchplay "$u"; \
        loginctl enable-linger "$u" 2>/dev/null || true; \
        mkdir -p "/home/$u/.config/pipewire/pipewire-pulse.conf.d"; \
        cp data/pipewire/50-couchplay.conf \
           "/home/$u/.config/pipewire/pipewire-pulse.conf.d/"; \
        chown -R "$u:$u" "/home/$u/.config"; \
    done

ENV QT_QPA_PLATFORM=wayland \
    PATH=/opt/e2e-venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

COPY appiumtests/container/entrypoint.sh /entrypoint.sh
COPY appiumtests/container/run-in-session.sh /src/couchplay/appiumtests/container/run-in-session.sh
RUN chmod +x /entrypoint.sh /src/couchplay/appiumtests/container/run-in-session.sh

ENTRYPOINT ["/entrypoint.sh"]
CMD ["appiumtests/"]
